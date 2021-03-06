/*
 * Notmuch support for mutt
 *
 * Copyright (C) 2011, 2012 Karel Zak <kzak@redhat.com>
 *
 * Notes:
 *
 * - notmuch uses private CONTEXT->data and private HEADER->data
 *
 * - all exported functions are usable within notmuch context only
 *
 * - all functions have to be covered by "ctx->magic == M_NOTMUCH" check
 *   (it's implemented in get_ctxdata() and init_context() functions).
 *
 * - exception are nm_nonctx_* functions -- these functions use nm_default_uri
 *   (or parse URI from another resourse)
 */
#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "mx.h"
#include "rfc2047.h"
#include "sort.h"
#include "mailbox.h"
#include "copy.h"
#include "keymap.h"
#include "url.h"
#include "buffy.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <utime.h>

#include <notmuch.h>

#include "mutt_notmuch.h"
#include "mutt_curses.h"

/* read whole-thread or maching messages only? */
enum {
	NM_QUERY_TYPE_MESGS = 0,	/* default */
	NM_QUERY_TYPE_THREADS
};

/*
 * Parsed URI arguments
 */
struct uri_tag {
	char *name;
	char *value;
	struct uri_tag *next;
};

/*
 * HEADER->data
 */
struct nm_hdrdata {
	char *folder;
	char *tags;
	char *oldpath;
	int magic;
};

/*
 * CONTEXT->data
 */
struct nm_ctxdata {
	notmuch_database_t *db;

	char *db_filename;
	char *db_query;
	int db_limit;
	int query_type;
	int longrun;

	struct uri_tag *query_items;
	progress_t *progress;
};

static void url_free_tags(struct uri_tag *tags)
{
	while (tags) {
		struct uri_tag *next = tags->next;
		FREE(&tags->name);
		FREE(&tags->value);
		FREE(&tags);
		tags = next;
	}
}

static int url_parse_query(char *url, char **filename, struct uri_tag **tags)
{
	char *p = strstr(url, "://");	/* remote unsupported */
	char *e;
	struct uri_tag *tag, *last = NULL;

	*filename = NULL;
	*tags = NULL;

	if (!p || !*(p + 3))
		return -1;

	p += 3;
	*filename = p;

	e = strchr(p, '?');

	*filename = e ? e == p ? NULL : strndup(p, e - p) : safe_strdup(p);
	if (!e)
		return 0;

	if (*filename && url_pct_decode(*filename) < 0)
		goto err;
	if (!e)
		return 0;	/* only filename */

	++e;	/* skip '?' */
	p = e;

	while (p && *p) {
		tag = safe_calloc(1, sizeof(struct uri_tag));
		if (!tag)
			goto err;

		if (!*tags)
			last = *tags = tag;
		else {
			last->next = tag;
			last = tag;
		}

		e = strchr(p, '=');
		if (!e)
			e = strchr(p, '&');
		tag->name = e ? strndup(p, e - p) : safe_strdup(p);
		if (!tag->name || url_pct_decode(tag->name) < 0)
			goto err;
		if (!e)
			break;

		p = e + 1;

		if (*e == '&')
			continue;

		e = strchr(p, '&');
		tag->value = e ? strndup(p, e - p) : safe_strdup(p);
		if (!tag->value || url_pct_decode(tag->value) < 0)
			goto err;
		if (!e)
			break;
		p = e + 1;
	}

	return 0;
err:
	FREE(&(*filename));
	url_free_tags(*tags);
	return -1;
}

static void free_hdrdata(struct nm_hdrdata *data)
{
	if (!data)
		return;

	dprint(2, (debugfile, "nm: freeing header %p\n", data));
	FREE(&data->folder);
	FREE(&data->tags);
	FREE(&data->oldpath);
	FREE(&data);
}

static void free_ctxdata(struct nm_ctxdata *data)
{
	if (!data)
		return;

	dprint(1, (debugfile, "nm: freeing context data %p\n", data));

	if (data->db)
#ifdef NOTMUCH_API_3
	        notmuch_database_destroy(data->db);
#else
		notmuch_database_close(data->db);
#endif
	data->db = NULL;

	FREE(&data->db_filename);
	FREE(&data->db_query);
	url_free_tags(data->query_items);
	FREE(&data);
}

static struct nm_ctxdata *new_ctxdata(char *uri)
{
	struct nm_ctxdata *data;

	if (!uri)
		return NULL;

	data = safe_calloc(1, sizeof(struct nm_ctxdata));
	dprint(1, (debugfile, "nm: initialize context data %p\n", data));

	data->db_limit = NotmuchDBLimit;

	if (url_parse_query(uri, &data->db_filename, &data->query_items)) {
		mutt_error(_("failed to parse notmuch uri: %s"), uri);
		data->db_filename = NULL;
		data->query_items = NULL;
		data->query_type = 0;
		return NULL;
	}

	return data;
}

static int deinit_context(CONTEXT *ctx)
{
	int i;

	if (!ctx || ctx->magic != M_NOTMUCH)
		return -1;

	for (i = 0; i < ctx->msgcount; i++) {
		HEADER *h = ctx->hdrs[i];

		if (h) {
			free_hdrdata(h->data);
			h->data = NULL;
		}
	}

	free_ctxdata(ctx->data);
	ctx->data = NULL;
	return 0;
}

static int init_context(CONTEXT *ctx)
{
	if (!ctx || ctx->magic != M_NOTMUCH)
		return -1;

	if (ctx->data)
		return 0;

	ctx->data = new_ctxdata(ctx->path);
	if (!ctx->data)
		return -1;

	ctx->mx_close = deinit_context;
	return 0;
}

char *nm_header_get_folder(HEADER *h)
{
	return h && h->data ? ((struct nm_hdrdata *) h->data)->folder : NULL;
}

char *nm_header_get_tags(HEADER *h)
{
	return h && h->data ? ((struct nm_hdrdata *) h->data)->tags : NULL;
}

int nm_header_get_magic(HEADER *h)
{
	return h && h->data ? ((struct nm_hdrdata *) h->data)->magic : 0;
}

/*
 * Returns (allocated) notmuch compatible message Id.
 */
static char *nm_header_get_id(HEADER *h)
{
	size_t sz;

	if (!h || !h->env || !h->env->message_id)
		return NULL;

	sz = strlen(h->env->message_id);

	/* remove '<' and '>' from id */
	return strndup(h->env->message_id + 1, sz - 2);
}


char *nm_header_get_fullpath(HEADER *h, char *buf, size_t bufsz)
{
	snprintf(buf, bufsz, "%s/%s", nm_header_get_folder(h), h->path);
	/*dprint(2, (debugfile, "nm: returns fullpath '%s'\n", buf));*/
	return buf;
}


static struct nm_ctxdata *get_ctxdata(CONTEXT *ctx)
{
	if (ctx && ctx->magic == M_NOTMUCH)
		return ctx->data;

	return NULL;
}

static char *get_query_string(struct nm_ctxdata *data)
{
	struct uri_tag *item;

	if (!data)
		return NULL;
	if (data->db_query)
		return data->db_query;

	for (item = data->query_items; item; item = item->next) {
		if (!item->value || !item->name)
			continue;

		if (strcmp(item->name, "limit") == 0) {
			if (mutt_atoi(item->value, &data->db_limit))
				mutt_error (_("failed to parse notmuch limit: %s"), item->value);

		} else if (strcmp(item->name, "type") == 0) {
			if (strcmp(item->value, "threads") == 0)
				data->query_type = NM_QUERY_TYPE_THREADS;
			else if (strcmp(item->value, "messages") == 0)
				data->query_type = NM_QUERY_TYPE_MESGS;
			else
				mutt_error (_("failed to parse notmuch query type: %s"), item->value);

		} else if (strcmp(item->name, "query") == 0)
			data->db_query = safe_strdup(item->value);
	}

	dprint(2, (debugfile, "nm: query '%s'\n", data->db_query));

	return data->db_query;
}

static int get_limit(struct nm_ctxdata *data)
{
	return data ? data->db_limit : 0;
}

static int get_query_type(struct nm_ctxdata *data)
{
	return data ? data->query_type : 0;
}

static const char *get_db_filename(struct nm_ctxdata *data)
{
	char *db_filename;

	if (!data)
		return NULL;

	db_filename = data->db_filename ? data->db_filename : NotmuchDefaultUri;
	if (!db_filename)
		db_filename = Maildir;
	if (!db_filename)
		return NULL;
	if (strncmp(db_filename, "notmuch://", 10) == 0)
		db_filename += 10;

	dprint(2, (debugfile, "nm: db filename '%s'\n", db_filename));
	return db_filename;
}

static notmuch_database_t *do_database_open(const char *filename,
					    int writable, int verbose)
{
	notmuch_database_t *db = NULL;
	unsigned int ct = 0;
	notmuch_status_t st = NOTMUCH_STATUS_SUCCESS;

	dprint(1, (debugfile, "nm: db open '%s' %s (timeout %d)\n", filename,
			writable ? "[WRITE]" : "[READ]", NotmuchOpenTimeout));
	do {
#ifdef NOTMUCH_API_3
		st = notmuch_database_open(filename,
					writable ? NOTMUCH_DATABASE_MODE_READ_WRITE :
					NOTMUCH_DATABASE_MODE_READ_ONLY, &db);
#else
		db = notmuch_database_open(filename,
					writable ? NOTMUCH_DATABASE_MODE_READ_WRITE :
					NOTMUCH_DATABASE_MODE_READ_ONLY);
#endif
		if (db || !NotmuchOpenTimeout || ct / 2 > NotmuchOpenTimeout)
			break;

		if (verbose && ct && ct % 2 == 0)
			mutt_error(_("Waiting for notmuch DB... (%d sec)"), ct / 2);
		usleep(500000);
		ct++;
	} while (1);

	if (verbose) {
		if (!db)
			mutt_error (_("Cannot open notmuch database: %s: %s"),
				    filename,
				    st ? notmuch_status_to_string(st) :
					 _("unknown reason"));
		else if (ct > 1)
			mutt_clear_error();
	}
	return db;
}

static notmuch_database_t *get_db(struct nm_ctxdata *data, int writable)
{
	if (!data)
	       return NULL;
	if (!data->db) {
		const char *db_filename = get_db_filename(data);

		if (db_filename)
			data->db = do_database_open(db_filename, writable, TRUE);
	}
	return data->db;
}

static int release_db(struct nm_ctxdata *data)
{
	if (data && data->db) {
		dprint(1, (debugfile, "nm: db close\n"));
#ifdef NOTMUCH_API_3
		notmuch_database_destroy(data->db);
#else
		notmuch_database_close(data->db);
#endif
		data->db = NULL;
		data->longrun = FALSE;
		return 0;
	}

	return -1;
}

void nm_longrun_init(CONTEXT *ctx, int writable)
{
	struct nm_ctxdata *data = get_ctxdata(ctx);

	if (data && get_db(data, writable)) {
		data->longrun = TRUE;
		dprint(2, (debugfile, "nm: long run initialied\n"));
	}
}

void nm_longrun_done(CONTEXT *ctx)
{
	struct nm_ctxdata *data = get_ctxdata(ctx);

	if (data && release_db(data) == 0)
		dprint(2, (debugfile, "nm: long run deinitialied\n"));
}

static int is_longrun(struct nm_ctxdata *data)
{
	return data && data->longrun;
}

void nm_debug_check(CONTEXT *ctx)
{
	struct nm_ctxdata *data = get_ctxdata(ctx);

	if (!data)
		return;

	if (data->db) {
		dprint(1, (debugfile, "nm: ERROR: db is open, closing\n"));
		release_db(data);
	}
}

static int get_database_mtime(struct nm_ctxdata *data, time_t *mtime)
{
	char path[_POSIX_PATH_MAX];
	struct stat st;

	if (!data)
	       return -1;

	snprintf(path, sizeof(path), "%s/.notmuch/xapian", get_db_filename(data));
	dprint(2, (debugfile, "nm: checking '%s' mtime\n", path));

	if (stat(path, &st))
		return -1;

	if (mtime)
		*mtime = st.st_mtime;

	return 0;
}

static notmuch_query_t *get_query(struct nm_ctxdata *data, int writable)
{
	notmuch_database_t *db = NULL;
	notmuch_query_t *q = NULL;
	const char *str;

	if (!data)
		return NULL;

	db = get_db(data, writable);
	str = get_query_string(data);

	if (!db || !str)
		goto err;

	q = notmuch_query_create(db, str);
	if (!q)
		goto err;

	notmuch_query_set_sort(q, NOTMUCH_SORT_NEWEST_FIRST);
	dprint(2, (debugfile, "nm: query succesfully initialized\n"));
	return q;
err:
	if (!is_longrun(data))
		release_db(data);
	return NULL;
}


static int update_header_tags(HEADER *h, notmuch_message_t *msg)
{
	struct nm_hdrdata *data = h->data;
	notmuch_tags_t *tags;
	char *tstr = NULL, *p;
	size_t sz = 0;

	dprint(2, (debugfile, "nm: tags update requested (%s)\n", h->env->message_id));

	for (tags = notmuch_message_get_tags(msg);
	     tags && notmuch_tags_valid(tags);
	     notmuch_tags_move_to_next(tags)) {

		const char *t = notmuch_tags_get(tags);
		size_t xsz = t ? strlen(t) : 0;

		if (!xsz)
			continue;

		if (NotmuchHiddenTags) {
			p = strstr(NotmuchHiddenTags, t);

			if (p && (p == NotmuchHiddenTags
				  || *(p - 1) == ','
				  || *(p - 1) == ' ')
			    && (*(p + xsz) == '\0'
				  || *(p + xsz) == ','
				  || *(p + xsz) == ' '))
				continue;
		}

		safe_realloc(&tstr, sz + (sz ? 1 : 0) + xsz + 1);
		p = tstr + sz;
		if (sz) {
			*p++ = ' ';
			sz++;
		}
		memcpy(p, t, xsz + 1);
		sz += xsz;
	}

	if (data->tags && tstr && strcmp(data->tags, tstr) == 0) {
		FREE(&tstr);
		dprint(2, (debugfile, "nm: tags unchanged\n"));
		return 1;
	}

	FREE(&data->tags);
	data->tags = tstr;
	dprint(2, (debugfile, "nm: new tags: '%s'\n", tstr));
	return 0;
}

/*
 * set/update HEADE->path and HEADER->data->path
 */
static int update_message_path(HEADER *h, const char *path)
{
	struct nm_hdrdata *data = h->data;
	char *p;

	dprint(2, (debugfile, "nm: path update requested path=%s, (%s)\n",
				path, h->env->message_id));

	p = strrchr(path, '/');
	if (p && p - path > 3 &&
	    (strncmp(p - 3, "cur", 3) == 0 ||
	     strncmp(p - 3, "new", 3) == 0 ||
	     strncmp(p - 3, "tmp", 3) == 0)) {

		data->magic = M_MAILDIR;

		FREE(&h->path);
		FREE(&data->folder);

		p -= 3;				/* skip subfolder (e.g. "new") */
		h->path = safe_strdup(p);

		for (; p > path && *(p - 1) == '/'; p--);

		data->folder = strndup(path, p - path);

		dprint(2, (debugfile, "nm: folder='%s', file='%s'\n", data->folder, h->path));
		return 0;
	}

	return 1;
}

static char *get_folder_from_path(const char *path)
{
	char *p = strrchr(path, '/');

	if (p && p - path > 3 &&
	    (strncmp(p - 3, "cur", 3) == 0 ||
	     strncmp(p - 3, "new", 3) == 0 ||
	     strncmp(p - 3, "tmp", 3) == 0)) {

		p -= 3;
		for (; p > path && *(p - 1) == '/'; p--);

		return strndup(path, p - path);
	}

	return NULL;
}

static void deinit_header(HEADER *h)
{
	if (h) {
		free_hdrdata(h->data);
		h->data = NULL;
	}
}

static int init_header(HEADER *h, const char *path, notmuch_message_t *msg)
{
	if (h->data)
		return 0;

	h->data = safe_calloc(1, sizeof(struct nm_hdrdata));
	h->free_cb = deinit_header;

	dprint(2, (debugfile, "nm: initialize header data: [hdr=%p, data=%p] (%s)\n",
				h, h->data, h->env->message_id));

	if (update_message_path(h, path))
		return -1;

	update_header_tags(h, msg);
	return 0;
}

/**
static void debug_print_filenames(notmuch_message_t *msg)
{
	notmuch_filenames_t *ls;
	const char *id = notmuch_message_get_message_id(msg);

	for (ls = notmuch_message_get_filenames(msg);
	     ls && notmuch_filenames_valid(ls);
	     notmuch_filenames_move_to_next(ls)) {

		dprint(2, (debugfile, "nm: %s: %s\n", id, notmuch_filenames_get(ls)));
	}
}

static void debug_print_tags(notmuch_message_t *msg)
{
	notmuch_tags_t *tags;
	const char *id = notmuch_message_get_message_id(msg);

	for (tags = notmuch_message_get_tags(msg);
	     tags && notmuch_tags_valid(tags);
	     notmuch_tags_move_to_next(tags)) {

		dprint(2, (debugfile, "nm: %s: %s\n", id, notmuch_tags_get(tags)));
	}
}
***/

static const char *get_message_last_filename(notmuch_message_t *msg)
{
	notmuch_filenames_t *ls;
	const char *name = NULL;

	for (ls = notmuch_message_get_filenames(msg);
	     ls && notmuch_filenames_valid(ls);
	     notmuch_filenames_move_to_next(ls)) {

		name = notmuch_filenames_get(ls);
	}

	return name;
}

static void append_message(CONTEXT *ctx, notmuch_message_t *msg)
{
	const char *path = get_message_last_filename(msg);
	char *newpath = NULL;
	HEADER *h = NULL;

	if (!path)
		return;

	dprint(2, (debugfile, "nm: appending message, i=%d, (%s)\n",
				ctx->msgcount,
				notmuch_message_get_message_id(msg)));

	if (ctx->msgcount >= ctx->hdrmax) {
		dprint(2, (debugfile, "nm: allocate mx memory\n"));
		mx_alloc_memory(ctx);
	}
	if (access(path, F_OK) == 0)
		h = maildir_parse_message(M_MAILDIR, path, 0, NULL);
	else {
		/* maybe moved try find it... */
		char *folder = get_folder_from_path(path);

		if (folder) {
			FILE *f = maildir_open_find_message(folder, path, &newpath);
			if (f) {
				h = maildir_parse_stream(M_MAILDIR, f, newpath, 0, NULL);
				fclose(f);

				dprint(1, (debugfile, "nm: not up-to-date: %s -> %s\n",
							path, newpath));
			}
		}
		FREE(&folder);
	}

	if (!h) {
		dprint(1, (debugfile, "nm: failed to parse message: %s\n", path));
		goto done;
	}
	if (init_header(h, newpath ? newpath : path, msg) != 0) {
		mutt_free_header(&h);
		dprint(1, (debugfile, "nm: failed to append header!\n"));
		goto done;
	}

	h->active = 1;
	h->index = ctx->msgcount;
	ctx->size += h->content->length
		   + h->content->offset
		   - h->content->hdr_offset;
	ctx->hdrs[ctx->msgcount] = h;
	ctx->msgcount++;

	if (newpath) {
		/* remember that file has been moved -- nm_sync() will update the DB */
		struct nm_hdrdata *hd = (struct nm_hdrdata *) h->data;

		if (hd) {
			dprint(1, (debugfile, "nm: remember obsolete path: %s\n", path));
			hd->oldpath = safe_strdup(path);
		}
	}
done:
	FREE(&newpath);
}

/*
 * add all the replies to a given messages into the display.
 * Careful, this calls itself recursively to make sure we get
 * everything.
 */
static void append_replies(CONTEXT *ctx, notmuch_message_t *top)
{
	notmuch_messages_t *msgs;

	for (msgs = notmuch_message_get_replies(top);
	     notmuch_messages_valid(msgs);
	     notmuch_messages_move_to_next(msgs)) {

		notmuch_message_t *m = notmuch_messages_get(msgs);
		append_message(ctx, m);
		if (!ctx->quiet)
		  mutt_progress_update (get_ctxdata(ctx)->progress, ctx->msgcount, -1);
		/* recurse through all the replies to this message too */
		append_replies(ctx, m);
		notmuch_message_destroy(m);
	}
}

/*
 * add each top level reply in the thread, and then add each
 * reply to the top level replies
 */
static void append_thread(CONTEXT *ctx, notmuch_thread_t *thread)
{
	notmuch_messages_t *msgs;

	for (msgs = notmuch_thread_get_toplevel_messages(thread);
	     notmuch_messages_valid(msgs);
	     notmuch_messages_move_to_next(msgs)) {

		notmuch_message_t *m = notmuch_messages_get(msgs);
		append_message(ctx, m);
		if (!ctx->quiet)
		  mutt_progress_update (get_ctxdata(ctx)->progress, ctx->msgcount, -1);
		append_replies(ctx, m);
		notmuch_message_destroy(m);
	}
}

static void read_mesgs_query(CONTEXT *ctx, notmuch_query_t *q)
{
	struct nm_ctxdata *data = get_ctxdata(ctx);
	int limit;
	notmuch_messages_t *msgs;

	if (!data)
		return;

	limit = get_limit(data);

	for (msgs = notmuch_query_search_messages(q);
	     notmuch_messages_valid(msgs) &&
		(limit == 0 || ctx->msgcount < limit);
	     notmuch_messages_move_to_next(msgs)) {

		notmuch_message_t *m = notmuch_messages_get(msgs);
		append_message(ctx, m);
		if (!ctx->quiet)
		  mutt_progress_update (get_ctxdata(ctx)->progress, ctx->msgcount, -1);
		notmuch_message_destroy(m);
	}
}

static void read_threads_query(CONTEXT *ctx, notmuch_query_t *q)
{
	struct nm_ctxdata *data = get_ctxdata(ctx);
	int limit;
	notmuch_threads_t *threads;

	if (!data)
		return;

	limit = get_limit(data);

	for (threads = notmuch_query_search_threads(q);
	     notmuch_threads_valid(threads) &&
		(limit == 0 || ctx->msgcount < limit);
	     notmuch_threads_move_to_next(threads)) {

		notmuch_thread_t *thread = notmuch_threads_get(threads);
		append_thread(ctx, thread);
		notmuch_thread_destroy(thread);
	}
}

int nm_read_query(CONTEXT *ctx)
{
	notmuch_query_t *q;
	struct nm_ctxdata *data;
	int rc = -1;
	char msgbuf[STRING];
	progress_t progress;

	if (init_context(ctx) != 0)
		return -1;

	data = get_ctxdata(ctx);
	if (!data)
		return -1;

	data->progress = &progress;

	dprint(1, (debugfile, "nm: reading messages...[current count=%d]\n",
				ctx->msgcount));
	if (!ctx->quiet) {
	  snprintf (msgbuf, sizeof (msgbuf), _("Reading %s..."), ctx->path);
	  mutt_progress_init (data->progress, msgbuf, M_PROGRESS_MSG, ReadInc, 0);
	}

	q = get_query(data, FALSE);
	if (q) {
		int type = get_query_type(data);

		switch(type) {
		case NM_QUERY_TYPE_MESGS:
			read_mesgs_query(ctx, q);
			break;
		case NM_QUERY_TYPE_THREADS:
			read_threads_query(ctx, q);
			break;
		}
		notmuch_query_destroy(q);
		rc = 0;

	}

	if (!is_longrun(data))
		release_db(data);

	ctx->mtime = time(NULL);

	mx_update_context(ctx, ctx->msgcount);

	dprint(1, (debugfile, "nm: reading messages... done [rc=%d, count=%d]\n",
				rc, ctx->msgcount));
	return rc;
}

char *nm_uri_from_query(CONTEXT *ctx, char *buf, size_t bufsz)
{
	struct nm_ctxdata *data = get_ctxdata(ctx);
	char uri[_POSIX_PATH_MAX];

	if (data)
		snprintf(uri, sizeof(uri), "notmuch://%s?query=%s",
			 get_db_filename(data), buf);
	else if (NotmuchDefaultUri)
		snprintf(uri, sizeof(uri), "%s?query=%s", NotmuchDefaultUri, buf);
	else if (Maildir)
		snprintf(uri, sizeof(uri), "notmuch://%s?query=%s", Maildir, buf);
	else
		return NULL;

	strncpy(buf, uri, bufsz);
	buf[bufsz - 1] = '\0';

	dprint(1, (debugfile, "nm: uri from query '%s'\n", buf));
	return buf;
}

/*
 * returns message from notmuch database
 */
static notmuch_message_t *get_nm_message(notmuch_database_t *db, HEADER *hdr)
{
	notmuch_message_t *msg = NULL;
	char *id = nm_header_get_id(hdr);

	dprint(2, (debugfile, "nm: find message (%s)\n", id));

	if (id && db)
		notmuch_database_find_message(db, id, &msg);

	FREE(&id);
	return msg;
}

static int update_tags(notmuch_message_t *msg, const char *tags)
{
	char *tag = NULL, *end = NULL, *p;
	char *buf = safe_strdup(tags);

	if (!buf)
		return -1;

	notmuch_message_freeze(msg);

	for (p = buf; p && *p; p++) {
		if (!tag && isspace(*p))
			continue;
		if (!tag)
			tag = p;		/* begin of the tag */
		if (*p == ',' || *p == ' ')
			end = p;		/* terminate the tag */
		else if (*(p + 1) == '\0')
			end = p + 1;		/* end of optstr */
		if (!tag || !end)
			continue;
		if (tag >= end)
			break;

		*end = '\0';

		if (*tag == '-') {
			dprint(1, (debugfile, "nm: remove tag: '%s'\n", tag + 1));
			notmuch_message_remove_tag(msg, tag + 1);
		} else {
			dprint(1, (debugfile, "nm: add tag: '%s'\n", *tag == '+' ? tag + 1 : tag));
			notmuch_message_add_tag(msg, *tag == '+' ? tag + 1 : tag);
		}
		end = tag = NULL;
	}

	notmuch_message_thaw(msg);
	FREE(&buf);
	return 0;
}

int nm_modify_message_tags(CONTEXT *ctx, HEADER *hdr, char *buf)
{
	struct nm_ctxdata *data = get_ctxdata(ctx);
	notmuch_database_t *db = NULL;
	notmuch_message_t *msg = NULL;
	int rc = -1;

	if (!buf || !*buf || !data)
		return -1;

	if (!(db = get_db(data, TRUE)) || !(msg = get_nm_message(db, hdr)))
		goto done;

	dprint(1, (debugfile, "nm: tags modify: '%s'\n", buf));

	update_tags(msg, buf);
	update_header_tags(hdr, msg);

	rc = 0;
	hdr->changed = TRUE;
done:
	if (!is_longrun(data))
		release_db(data);
	if (hdr->changed)
		ctx->mtime = time(NULL);
	dprint(1, (debugfile, "nm: tags modify done [rc=%d]\n", rc));
	return rc;
}

static int rename_maildir_filename(const char *old, char *newpath, size_t newsz, HEADER *h)
{
	char filename[_POSIX_PATH_MAX];
	char suffix[_POSIX_PATH_MAX];
	char folder[_POSIX_PATH_MAX];
	char *p;

	strfcpy(folder, old, sizeof(folder));
	p = strrchr(folder, '/');
	if (p)
		*p = '\0';

	p++;
	strfcpy(filename, p, sizeof(filename));

	/* remove (new,cur,...) from folder path */
	p = strrchr(folder, '/');
	if (p)
		*p = '\0';

	/* remove old flags from filename */
	if ((p = strchr(filename, ':')))
		*p = '\0';

	/* compose new flags */
	maildir_flags(suffix, sizeof(suffix), h);

	snprintf(newpath, newsz, "%s/%s/%s%s",
			folder,
			(h->read || h->old) ? "cur" : "new",
			filename,
			suffix);

	if (strcmp(old, newpath) == 0)
		return 1;

	if (rename(old, newpath) != 0) {
		dprint(1, (debugfile, "nm: rename(2) failed %s -> %s\n", old, newpath));
		return -1;
	}

	return 0;
}

static int remove_filename(notmuch_database_t *db, const char *path)
{
	notmuch_status_t st;
	notmuch_filenames_t *ls;
	notmuch_message_t *msg = NULL;

	dprint(2, (debugfile, "nm: remove filename '%s'\n", path));

	st = notmuch_database_begin_atomic(db);
	if (st)
		return -1;

	st = notmuch_database_find_message_by_filename(db, path, &msg);
	if (st || !msg)
		return -1;

	/*
	 * note that unlink() is probably unecessary here, it's already removed
	 * by mh_sync_mailbox_message(), but for sure...
	 */
	st = notmuch_database_remove_message(db, path);
	switch (st) {
	case NOTMUCH_STATUS_SUCCESS:
		unlink(path);
		break;
	case NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID:
		unlink(path);
		for (ls = notmuch_message_get_filenames(msg);
		     ls && notmuch_filenames_valid(ls);
		     notmuch_filenames_move_to_next(ls)) {

			path = notmuch_filenames_get(ls);

			dprint(2, (debugfile, "nm: remove duplicate: '%s'\n", path));
			unlink(path);
			notmuch_database_remove_message(db, path);
		}
		break;
	default:
		dprint(1, (debugfile, "nm: failed to remove '%s' [st=%d]\n", path, (int) st));
		break;
	}

	notmuch_message_destroy(msg);
	notmuch_database_end_atomic(db);
	return 0;
}

static int rename_filename(notmuch_database_t *db,
			const char *old, const char *new, HEADER *h)
{
	int rc = -1;
	notmuch_status_t st;
	notmuch_filenames_t *ls;
	notmuch_message_t *msg;

	if (!db || !new || !old || access(new, F_OK) != 0)
		return -1;

	dprint(1, (debugfile, "nm: rename filename, %s -> %s\n", old, new));
	st = notmuch_database_begin_atomic(db);
	if (st)
		return -1;

	dprint(2, (debugfile, "nm: rename: add '%s'\n", new));
	st = notmuch_database_add_message(db, new, &msg);

	if (st != NOTMUCH_STATUS_SUCCESS &&
	    st != NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID) {
		dprint(1, (debugfile, "nm: failed to add '%s' [st=%d]\n", new, (int) st));
		goto done;
	}

	dprint(2, (debugfile, "nm: rename: rem '%s'\n", old));
	st = notmuch_database_remove_message(db, old);
	switch (st) {
	case NOTMUCH_STATUS_SUCCESS:
		break;
	case NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID:
		dprint(2, (debugfile, "nm: rename: syncing duplicate filename\n"));
		notmuch_message_destroy(msg);
		msg = NULL;
		notmuch_database_find_message_by_filename(db, new, &msg);

		for (ls = notmuch_message_get_filenames(msg);
		     msg && ls && notmuch_filenames_valid(ls);
		     notmuch_filenames_move_to_next(ls)) {

			const char *path = notmuch_filenames_get(ls);
			char newpath[_POSIX_PATH_MAX];

			if (strcmp(new, path) == 0)
				continue;

			if (rename_maildir_filename(path, newpath, sizeof(newpath), h) == 0) {
				dprint(2, (debugfile, "nm: rename dup %s -> %s\n", path, newpath));
				notmuch_database_remove_message(db, path);
				notmuch_database_add_message(db, newpath, NULL);
			}
		}
		st = NOTMUCH_STATUS_SUCCESS;
		break;
	default:
		dprint(1, (debugfile, "nm: failed to remove '%s' [st=%d]\n",
					old, (int) st));
		break;
	}

	if (st == NOTMUCH_STATUS_SUCCESS && h && msg) {
		notmuch_message_maildir_flags_to_tags(msg);
		update_tags(msg, nm_header_get_tags(h));
	}

	rc = 0;
done:
	if (msg)
		notmuch_message_destroy(msg);
	notmuch_database_end_atomic(db);
	return rc;
}

int nm_update_filename(CONTEXT *ctx, const char *old, const char *new, HEADER *h)
{
	struct nm_ctxdata *data = get_ctxdata(ctx);

	if (!data || !new || !old)
		return -1;

	return rename_filename(get_db(data, TRUE), old, new, h);
}

int nm_sync(CONTEXT *ctx, int *index_hint)
{
	struct nm_ctxdata *data = get_ctxdata(ctx);
	int i, rc = 0;
	char msgbuf[STRING];
	progress_t progress;
	char *uri = ctx->path;
	notmuch_database_t *db = NULL;
	int changed = 0;

	if (!data)
		return -1;

	dprint(1, (debugfile, "nm: sync start ...\n"));

	if (!ctx->quiet) {
		snprintf(msgbuf, sizeof (msgbuf), _("Writing %s..."), ctx->path);
		mutt_progress_init(&progress, msgbuf, M_PROGRESS_MSG,
				   WriteInc, ctx->msgcount);
	}

	for (i = 0; i < ctx->msgcount; i++) {
		char old[_POSIX_PATH_MAX], new[_POSIX_PATH_MAX];
		HEADER *h = ctx->hdrs[i];
		struct nm_hdrdata *hd = h->data;

		if (!ctx->quiet)
			mutt_progress_update(&progress, i, -1);

		*old = *new = '\0';

		if (hd->oldpath) {
			strncpy(old, hd->oldpath, sizeof(old));
			old[sizeof(old) - 1] = '\0';
			dprint(2, (debugfile, "nm: fixing obsolete path '%s'\n", old));
		} else
			nm_header_get_fullpath(h, old, sizeof(old));

		ctx->path = hd->folder;
		ctx->magic = hd->magic;
#if USE_HCACHE
		rc = mh_sync_mailbox_message(ctx, i, NULL);
#else
		rc = mh_sync_mailbox_message(ctx, i);
#endif
		ctx->path = uri;
		ctx->magic = M_NOTMUCH;

		if (rc)
			break;

		if (!h->deleted)
			nm_header_get_fullpath(h, new, sizeof(new));

		if (h->deleted || strcmp(old, new) != 0) {
			/* email renamed or deleted -- update DB */
			if (!db) {
				db = get_db(data, TRUE);
				if (!db)
					break;
			}
			if (h->deleted && remove_filename(db, old) == 0)
				changed = 1;
			else if (*new && *old && rename_filename(db, old, new, h) == 0)
				changed = 1;
		}

		FREE(&hd->oldpath);
	}

	ctx->path = uri;
	ctx->magic = M_NOTMUCH;

	if (!is_longrun(data))
		release_db(data);
	if (changed)
		ctx->mtime = time(NULL);

	dprint(1, (debugfile, "nm: .... sync done [rc=%d]\n", rc));
	return rc;
}

static unsigned count_query(notmuch_database_t *db, const char *qstr)
{
	unsigned res = 0;
	notmuch_query_t *q = notmuch_query_create(db, qstr);

	if (q) {
		res = notmuch_query_count_messages(q);
		notmuch_query_destroy(q);
		dprint(1, (debugfile, "nm: count '%s', result=%d\n", qstr, res));
	}
	return res;
}

int nm_nonctx_get_count(char *path, int *all, int *new)
{
	struct uri_tag *query_items = NULL, *item;
	char *db_filename = NULL, *db_query = NULL;
	notmuch_database_t *db = NULL;
	int rc = -1, dflt = 0;

	dprint(1, (debugfile, "nm: count\n"));

	if (url_parse_query(path, &db_filename, &query_items)) {
		mutt_error(_("failed to parse notmuch uri: %s"), path);
		goto done;
	}
	if (!query_items)
		goto done;

	for (item = query_items; item; item = item->next) {
		if (item->value && strcmp(item->name, "query") == 0) {
			db_query = item->value;
			break;
		}
	}

	if (!db_query)
		goto done;

	if (!db_filename) {
		if (NotmuchDefaultUri) {
			if (strncmp(NotmuchDefaultUri, "notmuch://", 10) == 0)
				db_filename = NotmuchDefaultUri + 10;
			else
				db_filename = NotmuchDefaultUri;
		} else if (Maildir)
			db_filename = Maildir;
		dflt = 1;
	}

	/* don't be verbose about connection, as we're called from
	 * sidebar/buffy very often */
	db = do_database_open(db_filename, FALSE, FALSE);
	if (!db)
		goto done;

	/* all emails */
	if (all)
		*all = count_query(db, db_query);

	/* new messages */
	if (new) {
		if (strstr(db_query, NotmuchUnreadTag))
			*new = all ? *all : count_query(db, db_query);
		else {
			size_t qsz = strlen(db_query)
					+ sizeof(" and tag:")
					+ strlen(NotmuchUnreadTag);
			char *qstr = safe_malloc(qsz);

			if (!qstr)
				goto done;

			snprintf(qstr, qsz, "%s and tag:%s", db_query, NotmuchUnreadTag);
			*new = count_query(db, qstr);
			FREE(&qstr);
		}
	}

	rc = 0;
done:
	if (db) {
		notmuch_database_close(db);
		dprint(1, (debugfile, "nm: count close DB\n"));
	}
	if (!dflt)
		FREE(&db_filename);
	url_free_tags(query_items);

	dprint(1, (debugfile, "nm: count done [rc=%d]\n", rc));
	return rc;
}

char *nm_get_description(CONTEXT *ctx)
{
	BUFFY *p;

	for (p = VirtIncoming; p; p = p->next)
		if (p->path && p->desc && strcmp(p->path, ctx->path) == 0)
			return p->desc;

	return NULL;
}

/*
 * returns header from mutt context
 */
static HEADER *get_mutt_header(CONTEXT *ctx, notmuch_message_t *msg, char **mid)
{
	const char *id;
	size_t sz;

	if (!ctx || !msg || !mid)
		return NULL;

	id = notmuch_message_get_message_id(msg);
	if (!id)
		return NULL;

	dprint(2, (debugfile, "nm: mutt header, id='%s'\n", id));

	if (!ctx->id_hash) {
		dprint(2, (debugfile, "nm: init hash\n"));
		ctx->id_hash = mutt_make_id_hash(ctx);
		if (!ctx->id_hash)
			return NULL;
	}

	sz = strlen(id) + 3;
	safe_realloc(mid, sz);

	snprintf(*mid, sz, "<%s>", id);

	dprint(2, (debugfile, "nm: mutt id='%s'\n", *mid));
	return hash_find(ctx->id_hash, *mid);
}

int nm_check_database(CONTEXT *ctx, int *index_hint)
{
	struct nm_ctxdata *data = get_ctxdata(ctx);
	time_t mtime = 0;
	notmuch_query_t *q;
	notmuch_messages_t *msgs;
	int i, limit, oldmsgcount = 0, occult = 0, new_flags = 0;
	char *id = NULL;

	if (!data || get_database_mtime(data, &mtime) != 0)
		return -1;

	if (ctx->mtime >= mtime) {
		dprint(2, (debugfile, "nm: check unnecessary (db=%d ctx=%d)\n", mtime, ctx->mtime));
		return 0;
	}

	dprint(1, (debugfile, "nm: checking (db=%d ctx=%d)\n", mtime, ctx->mtime));

	q = get_query(data, FALSE);
	if (!q)
		goto done;

	dprint(1, (debugfile, "nm: start checking (count=%d)\n", ctx->msgcount));
	oldmsgcount = ctx->msgcount;

	for (i = 0; i < ctx->msgcount; i++)
		ctx->hdrs[i]->active = 0;

	limit = get_limit(data);

	for (i = 0, msgs = notmuch_query_search_messages(q);
	     notmuch_messages_valid(msgs) && (limit == 0 || i < limit);
	     notmuch_messages_move_to_next(msgs), i++) {

		char old[_POSIX_PATH_MAX];
		const char *new;

		notmuch_message_t *m = notmuch_messages_get(msgs);
		HEADER *h = get_mutt_header(ctx, m, &id);

		if (!h) {
			/* new email */
			append_message(ctx, m);
			continue;
		}

		/* message already exists, merge flags */
		h->active = 1;

		/* check to see if the message has moved to a different
		 * subdirectory.  If so, update the associated filename.
		 */
		new = notmuch_message_get_filename(m);
		nm_header_get_fullpath(h, old, sizeof(old));

		if (mutt_strcmp(old, new) != 0)
			update_message_path(h, new);

		if (!h->changed) {
			/* if the user hasn't modified the flags on
			 * this message, update the flags we just
			 * detected.
			 */
			HEADER tmp;
			memset(&tmp, 0, sizeof(tmp));
			maildir_parse_flags(&tmp, new);
			maildir_update_flags(ctx, h, &tmp);
		}

		if (update_header_tags(h, m) == 0)
			new_flags++;
	}

	for (i = 0; i < ctx->msgcount; i++) {
		if (ctx->hdrs[i]->active == 0) {
			occult = 1;
			break;
		}
	}

	if (ctx->msgcount > oldmsgcount)
		mx_update_context(ctx, ctx->msgcount - oldmsgcount);
done:
	if (!is_longrun(data))
		release_db(data);

	ctx->mtime = time(NULL);

	dprint(1, (debugfile, "nm: ... check done [count=%d, new_flags=%d, occult=%d]\n",
				ctx->msgcount, new_flags, occult));

	return occult ? M_REOPENED :
	       ctx->msgcount > oldmsgcount ? M_NEW_MAIL :
	       new_flags ? M_FLAGS : 0;
}
