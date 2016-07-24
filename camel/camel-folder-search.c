/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This library is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Michael Zucchi <notzed@ximian.com>
 */

/* This is a helper class for folders to implement the search function.
 * It implements enough to do basic searches on folders that can provide
 * an in-memory summary and a body index. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* POSIX requires <sys/types.h> be included before <regex.h> */
#include <sys/types.h>

#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>

#include <glib/gi18n-lib.h>

#include "camel-folder-search.h"
#include "camel-folder-thread.h"
#include "camel-iconv.h"
#include "camel-medium.h"
#include "camel-mime-message.h"
#include "camel-multipart.h"
#include "camel-search-private.h"
#include "camel-stream-mem.h"
#include "camel-db.h"
#include "camel-debug.h"
#include "camel-store.h"
#include "camel-vee-folder.h"
#include "camel-string-utils.h"
#include "camel-search-sql-sexp.h"

#define d(x)
#define r(x)
#define dd(x) if (camel_debug("search")) x

#define CAMEL_FOLDER_SEARCH_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), CAMEL_TYPE_FOLDER_SEARCH, CamelFolderSearchPrivate))

struct _CamelFolderSearchPrivate {
	GCancellable *cancellable;
	GError **error;

	CamelFolderThread *threads;
	GHashTable *threads_hash;
};

typedef enum {
	CAMEL_FOLDER_SEARCH_NONE = 0,
	CAMEL_FOLDER_SEARCH_ALWAYS_ENTER = 1 << 0,
	CAMEL_FOLDER_SEARCH_IMMEDIATE = 1 << 1
} CamelFolderSearchFlags;

static struct {
	const gchar *name;
	goffset offset;
	CamelFolderSearchFlags flags;
} builtins[] = {
	/* these have default implementations in CamelSExp */

	{ "and",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, and_),
	  CAMEL_FOLDER_SEARCH_IMMEDIATE },

	{ "or",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, or_),
	  CAMEL_FOLDER_SEARCH_IMMEDIATE },

	/* we need to override this one though to implement an 'array not' */
	{ "not",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, not_),
	  CAMEL_FOLDER_SEARCH_NONE },

	{ "<",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, lt),
	  CAMEL_FOLDER_SEARCH_IMMEDIATE },

	{ ">",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, gt),
	  CAMEL_FOLDER_SEARCH_IMMEDIATE },

	{ "=",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, eq),
	  CAMEL_FOLDER_SEARCH_IMMEDIATE },

	/* these we have to use our own default if there is none */
	/* they should all be defined in the language? so it parses, or should they not?? */

	{ "match-all",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, match_all),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER |
	  CAMEL_FOLDER_SEARCH_IMMEDIATE },

	{ "match-threads",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, match_threads),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER |
	  CAMEL_FOLDER_SEARCH_IMMEDIATE },

	{ "body-contains",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, body_contains),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "body-regex",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, body_regex),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "header-contains",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, header_contains),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "header-matches",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, header_matches),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "header-starts-with",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, header_starts_with),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "header-ends-with",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, header_ends_with),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "header-exists",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, header_exists),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "header-soundex",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, header_soundex),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "header-regex",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, header_regex),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "header-full-regex",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, header_full_regex),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "user-tag",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, user_tag),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "user-flag",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, user_flag),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "system-flag",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, system_flag),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "get-sent-date",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, get_sent_date),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "get-received-date",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, get_received_date),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "get-current-date",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, get_current_date),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "get-relative-months",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, get_relative_months),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "get-size",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, get_size),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "uid",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, uid),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },

	{ "message-location",
	  G_STRUCT_OFFSET (CamelFolderSearchClass, message_location),
	  CAMEL_FOLDER_SEARCH_ALWAYS_ENTER },
};

G_DEFINE_TYPE (CamelFolderSearch, camel_folder_search, G_TYPE_OBJECT)

/* this is just to OR results together */
struct IterData {
	gint count;
	GPtrArray *uids;
};

/* or, store all unique values */
static void
htor (gchar *key,
      gint value,
      struct IterData *iter_data)
{
	g_ptr_array_add (iter_data->uids, key);
}

/* and, only store duplicates */
static void
htand (gchar *key,
       gint value,
       struct IterData *iter_data)
{
	if (value == iter_data->count)
		g_ptr_array_add (iter_data->uids, key);
}

static void
add_thread_results (CamelFolderThreadNode *root,
                    GHashTable *result_hash)
{
	while (root) {
		g_hash_table_insert (result_hash, (gchar *) camel_message_info_uid (root->message), GINT_TO_POINTER (1));
		if (root->child)
			add_thread_results (root->child, result_hash);
		root = root->next;
	}
}

static void
add_results (gchar *uid,
             gpointer dummy,
             GPtrArray *result)
{
	g_ptr_array_add (result, uid);
}

static void
fill_thread_table (CamelFolderThreadNode *root,
                   GHashTable *id_hash)
{
	while (root) {
		g_hash_table_insert (id_hash, (gchar *) camel_message_info_uid (root->message), root);
		if (root->child)
			fill_thread_table (root->child, id_hash);
		root = root->next;
	}
}

static CamelMimeMessage *
get_current_message (CamelFolderSearch *search)
{
	if (!search || !search->folder || !search->current)
		return NULL;

	return camel_folder_get_message_sync (
		search->folder, search->current->uid, search->priv->cancellable, NULL);
}

static CamelSExpResult *
check_header (CamelSExp *sexp,
              gint argc,
              CamelSExpResult **argv,
              CamelFolderSearch *search,
              camel_search_match_t how)
{
	CamelSExpResult *r;
	gint truth = FALSE;

	r (printf ("executing check-header %d\n", how));

	/* are we inside a match-all? */
	if (search->current && argc > 1
	    && argv[0]->type == CAMEL_SEXP_RES_STRING
	    && !g_cancellable_is_cancelled (search->priv->cancellable)) {
		gchar *headername;
		const gchar *header = NULL, *charset = NULL;
		gchar strbuf[32];
		gint i, j;
		camel_search_t type = CAMEL_SEARCH_TYPE_ASIS;
		struct _camel_search_words *words;
		CamelMimeMessage *message = NULL;
		struct _camel_header_raw *raw_header;

		/* only a subset of headers are supported .. */
		headername = argv[0]->value.string;
		if (!g_ascii_strcasecmp (headername, "subject")) {
			header = camel_message_info_subject (search->current);
		} else if (!g_ascii_strcasecmp (headername, "date")) {
			/* FIXME: not a very useful form of the date */
			g_snprintf (
				strbuf, sizeof (strbuf), "%d",
				(gint) camel_message_info_date_sent (search->current));
			header = strbuf;
		} else if (!g_ascii_strcasecmp (headername, "from")) {
			header = camel_message_info_from (search->current);
			type = CAMEL_SEARCH_TYPE_ADDRESS;
		} else if (!g_ascii_strcasecmp (headername, "to")) {
			header = camel_message_info_to (search->current);
			type = CAMEL_SEARCH_TYPE_ADDRESS;
		} else if (!g_ascii_strcasecmp (headername, "cc")) {
			header = camel_message_info_cc (search->current);
			type = CAMEL_SEARCH_TYPE_ADDRESS;
		} else if (!g_ascii_strcasecmp (headername, "x-camel-mlist")) {
			header = camel_message_info_mlist (search->current);
			type = CAMEL_SEARCH_TYPE_MLIST;
		} else {
			message = get_current_message (search);
			if (message) {
				CamelContentType *ct = camel_mime_part_get_content_type (CAMEL_MIME_PART (message));

				if (ct) {
					charset = camel_content_type_param (ct, "charset");
					charset = camel_iconv_charset_name (charset);
				}
			}
		}

		if (header == NULL)
			header = "";

		/* performs an OR of all words */
		for (i = 1; i < argc && !truth; i++) {
			if (argv[i]->type == CAMEL_SEXP_RES_STRING) {
				if (argv[i]->value.string[0] == 0) {
					truth = TRUE;
				} else if (how == CAMEL_SEARCH_MATCH_CONTAINS) {
					/* Doesn't make sense to split words on
					 * anything but contains i.e. we can't
					 * have an ending match different words */
					words = camel_search_words_split ((const guchar *) argv[i]->value.string);
					truth = TRUE;
					for (j = 0; j < words->len && truth; j++) {
						if (message) {
							for (raw_header = ((CamelMimePart *) message)->headers; raw_header; raw_header = raw_header->next) {
								/* empty name means any header */
								if (!headername || !*headername || !g_ascii_strcasecmp (raw_header->name, headername)) {
									if (camel_search_header_match (raw_header->value, words->words[j]->word, how, type, charset))
										break;
								}
							}

							truth = raw_header != NULL;
						} else
							truth = camel_search_header_match (
								header,
								words->words[j]->word,
								how, type, charset);
					}
					camel_search_words_free (words);
				} else {
					if (message) {
						for (raw_header = ((CamelMimePart *) message)->headers; raw_header && !truth; raw_header = raw_header->next) {
							/* empty name means any header */
							if (!headername || !*headername || !g_ascii_strcasecmp (raw_header->name, headername)) {
								truth = camel_search_header_match (
									raw_header->value,
									argv[i]->value.string,
									how, type, charset);
							}
						}
					} else
						truth = camel_search_header_match (
							header,
							argv[i]->value.string,
							how, type, charset);
				}
			}
		}

		if (message)
			g_object_unref (message);
	}
	/* TODO: else, find all matches */

	r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
	r->value.boolean = truth;

	return r;
}

static gint
match_message_index (CamelIndex *idx,
                     const gchar *uid,
                     const gchar *match,
                     GError **error)
{
	CamelIndexCursor *wc, *nc;
	const gchar *word, *name;
	gint truth = FALSE;

	wc = camel_index_words (idx);
	if (wc) {
		while (!truth && (word = camel_index_cursor_next (wc))) {
			if (camel_ustrstrcase (word,match) != NULL) {
				/* perf: could have the wc cursor return the name cursor */
				nc = camel_index_find (idx, word);
				if (nc) {
					while (!truth && (name = camel_index_cursor_next (nc)))
						truth = strcmp (name, uid) == 0;
					g_object_unref (nc);
				}
			}
		}
		g_object_unref (wc);
	}

	return truth;
}

/*
 "one two" "three" "four five"
 *
 * one and two
 * or
 * three
 * or
 * four and five
 */

/* returns messages which contain all words listed in words */
static GPtrArray *
match_words_index (CamelFolderSearch *search,
                   struct _camel_search_words *words,
                   GCancellable *cancellable,
                   GError **error)
{
	GPtrArray *result = g_ptr_array_new ();
	struct IterData lambdafoo;
	CamelIndexCursor *wc, *nc;
	const gchar *word, *name;
	gint i;

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return result;

	/* we can have a maximum of 32 words, as we use it as the AND mask */

	wc = camel_index_words (search->body_index);
	if (wc) {
		GHashTable *ht = g_hash_table_new (g_str_hash, g_str_equal);

		while ((word = camel_index_cursor_next (wc))) {
			for (i = 0; i < words->len; i++) {
				if (camel_ustrstrcase (word, words->words[i]->word) != NULL) {
					/* perf: could have the wc cursor return the name cursor */
					nc = camel_index_find (search->body_index, word);
					if (nc) {
						while ((name = camel_index_cursor_next (nc))) {
								gint mask;

								mask = (GPOINTER_TO_INT (g_hash_table_lookup (ht, name))) | (1 << i);
								g_hash_table_insert (
									ht,
									(gchar *) camel_pstring_peek (name),
									GINT_TO_POINTER (mask));
						}
						g_object_unref (nc);
					}
				}
			}
		}
		g_object_unref (wc);

		lambdafoo.uids = result;
		lambdafoo.count = (1 << words->len) - 1;
		g_hash_table_foreach (ht, (GHFunc) htand, &lambdafoo);
		g_hash_table_destroy (ht);
	}

	return result;
}

static gboolean
match_words_1message (CamelDataWrapper *object,
                      struct _camel_search_words *words,
                      guint32 *mask,
                      GCancellable *cancellable)
{
	CamelDataWrapper *containee;
	gint truth = FALSE;
	gint parts, i;

	if (g_cancellable_is_cancelled (cancellable))
		return FALSE;

	containee = camel_medium_get_content (CAMEL_MEDIUM (object));

	if (containee == NULL)
		return FALSE;

	/* using the object types is more accurate than using the mime/types */
	if (CAMEL_IS_MULTIPART (containee)) {
		parts = camel_multipart_get_number (CAMEL_MULTIPART (containee));
		for (i = 0; i < parts && truth == FALSE; i++) {
			CamelDataWrapper *part = (CamelDataWrapper *) camel_multipart_get_part (CAMEL_MULTIPART (containee), i);
			if (part)
				truth = match_words_1message (part, words, mask, cancellable);
		}
	} else if (CAMEL_IS_MIME_MESSAGE (containee)) {
		/* for messages we only look at its contents */
		truth = match_words_1message ((CamelDataWrapper *) containee, words, mask, cancellable);
	} else if (camel_content_type_is (CAMEL_DATA_WRAPPER (containee)->mime_type, "text", "*")) {
		/* for all other text parts, we look inside, otherwise we dont care */
		CamelStream *stream;
		GByteArray *byte_array;
		const gchar *charset;

		byte_array = g_byte_array_new ();
		stream = camel_stream_mem_new_with_byte_array (byte_array);

		charset = camel_content_type_param (CAMEL_DATA_WRAPPER (containee)->mime_type, "charset");
		if (charset && *charset) {
			CamelMimeFilter *filter = camel_mime_filter_charset_new (charset, "UTF-8");
			if (filter) {
				CamelStream *filtered = camel_stream_filter_new (stream);

				if (filtered) {
					camel_stream_filter_add (CAMEL_STREAM_FILTER (filtered), filter);
					g_object_unref (stream);
					stream = filtered;
				}

				g_object_unref (filter);
			}
		}

		/* FIXME The match should be part of a stream op */
		camel_data_wrapper_decode_to_stream_sync (
			containee, stream, cancellable, NULL);
		camel_stream_write (stream, "", 1, NULL, NULL);
		for (i = 0; i < words->len; i++) {
			/* FIXME: This is horridly slow, and should use a real search algorithm */
			if (camel_ustrstrcase ((const gchar *) byte_array->data, words->words[i]->word) != NULL) {
				*mask |= (1 << i);
				/* shortcut a match */
				if (*mask == (1 << (words->len)) - 1)
					return TRUE;
			}
		}

		g_object_unref (stream);
	}

	return truth;
}

static gboolean
match_words_message (CamelFolder *folder,
                     const gchar *uid,
                     struct _camel_search_words *words,
                     GCancellable *cancellable,
                     GError **error)
{
	guint32 mask;
	CamelMimeMessage *msg;
	gint truth = FALSE;

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return truth;

	msg = camel_folder_get_message_sync (folder, uid, cancellable, error);
	if (msg) {
		mask = 0;
		truth = match_words_1message ((CamelDataWrapper *) msg, words, &mask, cancellable);
		g_object_unref (msg);
	}

	return truth;
}

static GPtrArray *
match_words_messages (CamelFolderSearch *search,
                      struct _camel_search_words *words,
                      GCancellable *cancellable,
                      GError **error)
{
	gint i;
	GPtrArray *matches = g_ptr_array_new ();

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return matches;

	if (search->body_index) {
		GPtrArray *indexed;
		struct _camel_search_words *simple;

		simple = camel_search_words_simple (words);
		indexed = match_words_index (search, simple, cancellable, error);
		camel_search_words_free (simple);

		for (i = 0; i < indexed->len && !g_cancellable_is_cancelled (cancellable); i++) {
			const gchar *uid = g_ptr_array_index (indexed, i);

			if (match_words_message (
					search->folder, uid, words,
					cancellable, error))
				g_ptr_array_add (matches, (gchar *) uid);
		}

		g_ptr_array_free (indexed, TRUE);
	} else {
		GPtrArray *v = search->summary_set ? search->summary_set : search->summary;

		for (i = 0; i < v->len && !g_cancellable_is_cancelled (cancellable); i++) {
			gchar *uid = g_ptr_array_index (v, i);

			if (match_words_message (
				search->folder, uid, words,
				cancellable, error))
				g_ptr_array_add (matches, (gchar *) uid);
		}
	}

	return matches;
}

static const gchar *
get_default_charset (CamelMimeMessage *msg)
{
	CamelContentType *ct;
	const gchar *charset;

	g_return_val_if_fail (msg != NULL, NULL);

	ct = camel_mime_part_get_content_type (CAMEL_MIME_PART (msg));
	charset = camel_content_type_param (ct, "charset");
	if (!charset)
		charset = "utf-8";

	charset = camel_iconv_charset_name (charset);

	return charset;
}

static gchar *
get_header_decoded (const gchar *header_value,
                    const gchar *default_charset)
{
	gchar *unfold, *decoded;

	if (!header_value || !*header_value)
		return NULL;

	unfold = camel_header_unfold (header_value);
	decoded = camel_header_decode_string (unfold, default_charset);
	g_free (unfold);

	return decoded;
}

static gchar *
get_full_header (CamelMimeMessage *message,
                 const gchar *default_charset)
{
	CamelMimePart *mp = CAMEL_MIME_PART (message);
	GString *str = g_string_new ("");
	struct _camel_header_raw *h;

	for (h = mp->headers; h; h = h->next) {
		if (h->value != NULL) {
			g_string_append (str, h->name);
			if (isspace (h->value[0]))
				g_string_append (str, ":");
			else
				g_string_append (str, ": ");
			if (g_ascii_strcasecmp (h->name, "From") == 0 ||
			    g_ascii_strcasecmp (h->name, "To") == 0 ||
			    g_ascii_strcasecmp (h->name, "CC") == 0 ||
			    g_ascii_strcasecmp (h->name, "BCC") == 0 ||
			    g_ascii_strcasecmp (h->name, "Subject") == 0) {
				gchar *decoded = get_header_decoded (h->value, default_charset);
				if (decoded)
					g_string_append (str, decoded);
				else
					g_string_append (str, h->value);
				g_free (decoded);
			} else {
				g_string_append (str, h->value);
			}
			g_string_append_c (str, '\n');
		}
	}

	return g_string_free (str, FALSE);
}

static gint
read_uid_callback (gpointer ref,
                   gint ncol,
                   gchar **cols,
                   gchar **name)
{
	GPtrArray *matches;

	matches = (GPtrArray *) ref;

	g_ptr_array_add (matches, (gpointer) camel_pstring_strdup (cols[0]));
	return 0;
}

/* dummy function, returns false always, or an empty match array */
static CamelSExpResult *
folder_search_dummy (CamelSExp *sexp,
                     gint argc,
                     CamelSExpResult **argv,
                     CamelFolderSearch *search)
{
	CamelSExpResult *r;

	if (search->current == NULL) {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
		r->value.boolean = FALSE;
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();
	}

	return r;
}

static CamelSExpResult *
folder_search_header_has_words (CamelSExp *sexp,
				gint argc,
				CamelSExpResult **argv,
				CamelFolderSearch *search)
{
	return check_header (sexp, argc, argv, search, CAMEL_SEARCH_MATCH_WORD);
}

static void
folder_search_dispose (GObject *object)
{
	CamelFolderSearch *search = CAMEL_FOLDER_SEARCH (object);

	if (search->sexp != NULL) {
		g_object_unref (search->sexp);
		search->sexp = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (camel_folder_search_parent_class)->dispose (object);
}

static void
folder_search_finalize (GObject *object)
{
	CamelFolderSearch *search = CAMEL_FOLDER_SEARCH (object);

	g_free (search->last_search);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (camel_folder_search_parent_class)->finalize (object);
}

static void
folder_search_constructed (GObject *object)
{
	CamelFolderSearch *search;
	CamelFolderSearchClass *class;
	gint ii;

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (camel_folder_search_parent_class)->constructed (object);

	search = CAMEL_FOLDER_SEARCH (object);
	class = CAMEL_FOLDER_SEARCH_GET_CLASS (search);

	/* Register class methods with the CamelSExp. */
	for (ii = 0; ii < G_N_ELEMENTS (builtins); ii++) {
		CamelFolderSearchFlags flags;
		const gchar *name;
		goffset offset;
		gpointer func;

		name = builtins[ii].name;
		flags = builtins[ii].flags;
		offset = builtins[ii].offset;

		/* c is sure messy sometimes */
		func = *((gpointer *)(((gchar *) class) + offset));

		if (func == NULL && flags & CAMEL_FOLDER_SEARCH_ALWAYS_ENTER) {
			g_warning (
				"%s doesn't implement '%s' method",
				G_OBJECT_TYPE_NAME (search), name);
			func = (gpointer) folder_search_dummy;
		}
		if (func != NULL) {
			if (flags & CAMEL_FOLDER_SEARCH_IMMEDIATE) {
				camel_sexp_add_ifunction (
					search->sexp, 0, name,
					(CamelSExpIFunc) func, search);
			} else {
				camel_sexp_add_function (
					search->sexp, 0, name,
					(CamelSExpFunc) func, search);
			}
		}
	}

	camel_sexp_add_function (
		search->sexp, 0, "header-has-words",
		(CamelSExpFunc) folder_search_header_has_words, search);
}

/* implement an 'array not', i.e. everything in the summary, not in the supplied array */
static CamelSExpResult *
folder_search_not (CamelSExp *sexp,
                   gint argc,
                   CamelSExpResult **argv,
                   CamelFolderSearch *search)
{
	CamelSExpResult *r;
	gint i;

	if (argc > 0) {
		if (argv[0]->type == CAMEL_SEXP_RES_ARRAY_PTR) {
			GPtrArray *v = argv[0]->value.ptrarray;
			const gchar *uid;

			r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
			r->value.ptrarray = g_ptr_array_new ();

			/* not against a single message?*/
			if (search->current) {
				gint found = FALSE;

				uid = camel_message_info_uid (search->current);
				for (i = 0; !found && i < v->len; i++) {
					if (strcmp (uid, v->pdata[i]) == 0)
						found = TRUE;
				}

				if (!found)
					g_ptr_array_add (r->value.ptrarray, (gchar *) uid);
			} else if (search->summary == NULL) {
				g_warning ("No summary set, 'not' against an array requires a summary");
			} else {
				/* 'not' against the whole summary */
				GHashTable *have = g_hash_table_new (g_str_hash, g_str_equal);
				gchar **s;
				gchar **m;

				s = (gchar **) v->pdata;
				for (i = 0; i < v->len; i++)
					g_hash_table_insert (have, s[i], s[i]);

				v = search->summary_set ? search->summary_set : search->summary;
				m = (gchar **) v->pdata;
				for (i = 0; i < v->len; i++) {
					gchar *uid = m[i];

					if (g_hash_table_lookup (have, uid) == NULL)
						g_ptr_array_add (r->value.ptrarray, uid);
				}
				g_hash_table_destroy (have);
			}
		} else {
			gint res = TRUE;

			if (argv[0]->type == CAMEL_SEXP_RES_BOOL)
				res = !argv[0]->value.boolean;

			r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
			r->value.boolean = res;
		}
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
		r->value.boolean = TRUE;
	}

	return r;
}

static CamelSExpResult *
folder_search_match_all (CamelSExp *sexp,
                         gint argc,
                         CamelSExpTerm **argv,
                         CamelFolderSearch *search)
{
	gint i;
	CamelSExpResult *r, *r1;
	gchar *error_msg;
	GPtrArray *v;

	if (argc > 1) {
		g_warning ("match-all only takes a single argument, other arguments ignored");
	}

	/* we are only matching a single message?  or already inside a match-all? */
	if (search->current) {
		d (printf ("matching against 1 message: %s\n", camel_message_info_subject (search->current)));

		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
		r->value.boolean = FALSE;

		if (argc > 0) {
			r1 = camel_sexp_term_eval (sexp, argv[0]);
			if (r1->type == CAMEL_SEXP_RES_BOOL) {
				r->value.boolean = r1->value.boolean;
			} else {
				g_warning ("invalid syntax, matches require a single bool result");
				/* Translators: The '%s' is an element type name, part of an expressing language */
				error_msg = g_strdup_printf (_("(%s) requires a single bool result"), "match-all");
				camel_sexp_fatal_error (sexp, error_msg);
				g_free (error_msg);
			}
			camel_sexp_result_free (sexp, r1);
		} else {
			r->value.boolean = TRUE;
		}
		return r;
	}

	r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
	r->value.ptrarray = g_ptr_array_new ();

	if (search->summary == NULL) {
		/* TODO: make it work - e.g. use the folder and so forth for a slower search */
		g_warning ("No summary supplied, match-all doesn't work with no summary");
		return r;
	}

	v = search->summary_set ? search->summary_set : search->summary;

	if (!CAMEL_IS_VEE_FOLDER (search->folder)) {
		camel_folder_summary_prepare_fetch_all (search->folder->summary, search->priv->error);
	}

	for (i = 0; i < v->len && !g_cancellable_is_cancelled (search->priv->cancellable); i++) {
		const gchar *uid;

		search->current = camel_folder_summary_get (search->folder->summary, v->pdata[i]);
		if (!search->current)
			continue;
		uid = camel_message_info_uid (search->current);

		if (argc > 0) {
			r1 = camel_sexp_term_eval (sexp, argv[0]);
			if (r1->type == CAMEL_SEXP_RES_BOOL) {
				if (r1->value.boolean)
					g_ptr_array_add (r->value.ptrarray, (gchar *) uid);
			} else {
				g_warning ("invalid syntax, matches require a single bool result");
				/* Translators: The '%s' is an element type name, part of an expressing language */
				error_msg = g_strdup_printf (_("(%s) requires a single bool result"), "match-all");
				camel_sexp_fatal_error (sexp, error_msg);
				g_free (error_msg);
			}
			camel_sexp_result_free (sexp, r1);
		} else {
			g_ptr_array_add (r->value.ptrarray, (gchar *) uid);
		}
		camel_message_info_unref (search->current);
	}
	search->current = NULL;
	return r;
}

static CamelSExpResult *
folder_search_match_threads (CamelSExp *sexp,
                             gint argc,
                             CamelSExpTerm **argv,
                             CamelFolderSearch *search)
{
	CamelSExpResult *r;
	CamelFolderSearchPrivate *p = search->priv;
	gint i, type;
	GHashTable *results;
	gchar *error_msg;

	if (g_cancellable_is_cancelled (search->priv->cancellable)) {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();
		return r;
	}

	/* not supported in match-all */
	if (search->current) {
		/* Translators: Each '%s' is an element type name, part of an expressing language */
		error_msg = g_strdup_printf (_("(%s) not allowed inside %s"), "match-threads", "match-all");
		camel_sexp_fatal_error (sexp, error_msg);
		g_free (error_msg);
	}

	if (argc == 0) {
		/* Translators: The '%s' is an element type name, part of an expressing language */
		error_msg = g_strdup_printf (_("(%s) requires a match type string"), "match-threads");
		camel_sexp_fatal_error (sexp, error_msg);
		g_free (error_msg);
	}

	r = camel_sexp_term_eval (sexp, argv[0]);
	if (r->type != CAMEL_SEXP_RES_STRING) {
		/* Translators: The '%s' is an element type name, part of an expressing language */
		error_msg = g_strdup_printf (_("(%s) requires a match type string"), "match-threads");
		camel_sexp_fatal_error (sexp, error_msg);
		g_free (error_msg);
	}

	type = 0;
	if (!strcmp (r->value.string, "none"))
		type = 0;
	else if (!strcmp (r->value.string, "all"))
		type = 1;
	else if (!strcmp (r->value.string, "replies"))
		type = 2;
	else if (!strcmp (r->value.string, "replies_parents"))
		type = 3;
	else if (!strcmp (r->value.string, "single"))
		type = 4;
	camel_sexp_result_free (sexp, r);

	/* behave as (begin does */
	r = NULL;
	for (i = 1; i < argc; i++) {
		if (r)
			camel_sexp_result_free (sexp, r);
		r = camel_sexp_term_eval (sexp, argv[i]);
	}

	if (r == NULL || r->type != CAMEL_SEXP_RES_ARRAY_PTR) {
		/* Translators: The '%s' is an element type name, part of an expressing language */
		error_msg = g_strdup_printf (_("(%s) expects an array result"), "match-threads");
		camel_sexp_fatal_error (sexp, error_msg);
		g_free (error_msg);
	}

	if (type == 0)
		return r;

	if (search->folder == NULL) {
		/* Translators: The '%s' is an element type name, part of an expressing language */
		error_msg = g_strdup_printf (_("(%s) requires the folder set"), "match-threads");
		camel_sexp_fatal_error (sexp, error_msg);
		g_free (error_msg);
	}

	/* cache this, so we only have to re-calculate once per search at most */
	if (p->threads == NULL) {
		p->threads = camel_folder_thread_new (search->folder, NULL, TRUE);
		p->threads_hash = g_hash_table_new (g_str_hash, g_str_equal);

		fill_thread_table (p->threads->tree, p->threads_hash);
	}

	results = g_hash_table_new (g_str_hash, g_str_equal);
	for (i = 0; i < r->value.ptrarray->len && !g_cancellable_is_cancelled (search->priv->cancellable); i++) {
		CamelFolderThreadNode *node, *scan;

		if (type != 4)
			g_hash_table_insert (results, g_ptr_array_index (r->value.ptrarray, i), GINT_TO_POINTER (1));
			
		node = g_hash_table_lookup (p->threads_hash, (gchar *) g_ptr_array_index (r->value.ptrarray, i));
		if (node == NULL) /* this shouldn't happen but why cry over spilt milk */
			continue;

		/* select messages in thread according to search criteria */
		if (type == 4) {
			if (node->child == NULL && node->parent == NULL)
				g_hash_table_insert (results, (gchar *) camel_message_info_uid (node->message), GINT_TO_POINTER (1));
		} else {
			if (type == 3) {
				scan = node;
				/* coverity[check_after_deref] */
				while (scan && scan->parent) {
					scan = scan->parent;
					g_hash_table_insert (results, (gchar *) camel_message_info_uid (scan->message), GINT_TO_POINTER (1));
				}
			} else if (type == 1) {
				while (node != NULL && node->parent)
					node = node->parent;
			}
			g_hash_table_insert (results, (gchar *) camel_message_info_uid (node->message), GINT_TO_POINTER (1));
			if (node->child)
				add_thread_results (node->child, results);
		}
	}
	camel_sexp_result_free (sexp, r);

	r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
	r->value.ptrarray = g_ptr_array_new ();

	g_hash_table_foreach (results, (GHFunc) add_results, r->value.ptrarray);
	g_hash_table_destroy (results);

	return r;
}

static CamelSExpResult *
folder_search_body_contains (CamelSExp *sexp,
                             gint argc,
                             CamelSExpResult **argv,
                             CamelFolderSearch *search)
{
	gint i, j;
	GError **error = search->priv->error;
	struct _camel_search_words *words;
	CamelSExpResult *r;
	struct IterData lambdafoo;

	if (search->current) {
		gint truth = FALSE;

		if (argc == 1 && argv[0]->value.string[0] == 0) {
			truth = TRUE;
		} else {
			for (i = 0; i < argc && !truth && !g_cancellable_is_cancelled (search->priv->cancellable); i++) {
				if (argv[i]->type == CAMEL_SEXP_RES_STRING) {
					words = camel_search_words_split ((const guchar *) argv[i]->value.string);
					truth = TRUE;
					if ((words->type & CAMEL_SEARCH_WORD_COMPLEX) == 0 && search->body_index) {
						for (j = 0; j < words->len && truth; j++)
							truth = match_message_index (
								search->body_index,
								camel_message_info_uid (search->current),
								words->words[j]->word,
								error);
					} else {
						/* TODO: cache current message incase of multiple body search terms */
						truth = match_words_message (
							search->folder,
							camel_message_info_uid (search->current),
							words,
							search->priv->cancellable,
							error);
					}
					camel_search_words_free (words);
				}
			}
		}
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
		r->value.boolean = truth;
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();

		if (argc == 1 && argv[0]->value.string[0] == 0) {
			GPtrArray *v = search->summary_set ? search->summary_set : search->summary;

			for (i = 0; i < v->len && !g_cancellable_is_cancelled (search->priv->cancellable); i++) {
				gchar *uid = g_ptr_array_index (v, i);

				g_ptr_array_add (r->value.ptrarray, uid);
			}
		} else {
			GHashTable *ht = g_hash_table_new (g_str_hash, g_str_equal);
			GPtrArray *matches;

			for (i = 0; i < argc && !g_cancellable_is_cancelled (search->priv->cancellable); i++) {
				if (argv[i]->type == CAMEL_SEXP_RES_STRING) {
					words = camel_search_words_split ((const guchar *) argv[i]->value.string);
					if ((words->type & CAMEL_SEARCH_WORD_COMPLEX) == 0 && search->body_index) {
						matches = match_words_index (search, words, search->priv->cancellable, error);
					} else {
						matches = match_words_messages (search, words, search->priv->cancellable, error);
					}
					for (j = 0; j < matches->len; j++) {
						g_hash_table_insert (ht, matches->pdata[j], matches->pdata[j]);
					}
					g_ptr_array_free (matches, TRUE);
					camel_search_words_free (words);
				}
			}
			lambdafoo.uids = r->value.ptrarray;
			g_hash_table_foreach (ht, (GHFunc) htor, &lambdafoo);
			g_hash_table_destroy (ht);
		}
	}

	return r;
}

static CamelSExpResult *
folder_search_body_regex (CamelSExp *sexp,
                          gint argc,
                          CamelSExpResult **argv,
                          CamelFolderSearch *search)
{
	CamelSExpResult *r;
	CamelMimeMessage *msg = get_current_message (search);

	if (msg) {
		regex_t pattern;

		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);

		if (!g_cancellable_is_cancelled (search->priv->cancellable) &&
		    camel_search_build_match_regex (
				&pattern,
				CAMEL_SEARCH_MATCH_ICASE |
				CAMEL_SEARCH_MATCH_REGEX |
				CAMEL_SEARCH_MATCH_NEWLINE,
				argc, argv,
				search->priv->error) == 0) {
			r->value.boolean = camel_search_message_body_contains ((CamelDataWrapper *) msg, &pattern);
			regfree (&pattern);
		} else
			r->value.boolean = FALSE;

		g_object_unref (msg);
	} else {
		regex_t pattern;

		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();

		if (!g_cancellable_is_cancelled (search->priv->cancellable) &&
		    camel_search_build_match_regex (
				&pattern,
				CAMEL_SEARCH_MATCH_ICASE |
				CAMEL_SEARCH_MATCH_REGEX |
				CAMEL_SEARCH_MATCH_NEWLINE,
				argc, argv,
				search->priv->error) == 0) {
			gint i;
			GPtrArray *v = search->summary_set ? search->summary_set : search->summary;
			CamelMimeMessage *message;

			for (i = 0; i < v->len && !g_cancellable_is_cancelled (search->priv->cancellable); i++) {
				gchar *uid = g_ptr_array_index (v, i);

				message = camel_folder_get_message_sync (
					search->folder, uid, search->priv->cancellable, NULL);
				if (message) {
					if (camel_search_message_body_contains ((CamelDataWrapper *) message, &pattern)) {
						g_ptr_array_add (r->value.ptrarray, uid);
					}

					g_object_unref (message);
				}
			}

			regfree (&pattern);
		}
	}

	return r;
}

static CamelSExpResult *
folder_search_header_contains (CamelSExp *sexp,
                               gint argc,
                               CamelSExpResult **argv,
                               CamelFolderSearch *search)
{
	return check_header (sexp, argc, argv, search, CAMEL_SEARCH_MATCH_CONTAINS);
}

static CamelSExpResult *
folder_search_header_matches (CamelSExp *sexp,
                              gint argc,
                              CamelSExpResult **argv,
                              CamelFolderSearch *search)
{
	return check_header (sexp, argc, argv, search, CAMEL_SEARCH_MATCH_EXACT);
}

static CamelSExpResult *
folder_search_header_starts_with (CamelSExp *sexp,
                                  gint argc,
                                  CamelSExpResult **argv,
                                  CamelFolderSearch *search)
{
	return check_header (sexp, argc, argv, search, CAMEL_SEARCH_MATCH_STARTS);
}

static CamelSExpResult *
folder_search_header_ends_with (CamelSExp *sexp,
                                gint argc,
                                CamelSExpResult **argv,
                                CamelFolderSearch *search)
{
	return check_header (sexp, argc, argv, search, CAMEL_SEARCH_MATCH_ENDS);
}

static CamelSExpResult *
folder_search_header_exists (CamelSExp *sexp,
                             gint argc,
                             CamelSExpResult **argv,
                             CamelFolderSearch *search)
{
	CamelSExpResult *r;

	r (printf ("executing header-exists\n"));

	if (search->current) {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
		if (argc == 1 && argv[0]->type == CAMEL_SEXP_RES_STRING)
			r->value.boolean = camel_medium_get_header (CAMEL_MEDIUM (search->current), argv[0]->value.string) != NULL;

	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();
	}

	return r;
}

static CamelSExpResult *
folder_search_header_soundex (CamelSExp *sexp,
                              gint argc,
                              CamelSExpResult **argv,
                              CamelFolderSearch *search)
{
	return check_header (sexp, argc, argv, search, CAMEL_SEARCH_MATCH_SOUNDEX);
}

static CamelSExpResult *
folder_search_header_regex (CamelSExp *sexp,
                            gint argc,
                            CamelSExpResult **argv,
                            CamelFolderSearch *search)
{
	CamelSExpResult *r;
	CamelMimeMessage *msg;

	msg = get_current_message (search);

	if (msg) {
		regex_t pattern;
		const gchar *contents;

		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);

		if (argc > 1 && argv[0]->type == CAMEL_SEXP_RES_STRING
		    && (contents = camel_medium_get_header (CAMEL_MEDIUM (msg), argv[0]->value.string))
		    && camel_search_build_match_regex (
				&pattern,
				CAMEL_SEARCH_MATCH_REGEX |
				CAMEL_SEARCH_MATCH_ICASE,
				argc - 1, argv + 1,
				search->priv->error) == 0) {
			gchar *decoded = NULL;
			const gchar *hader_name = argv[0]->value.string;

			if (g_ascii_strcasecmp (hader_name, "From") == 0 ||
			    g_ascii_strcasecmp (hader_name, "To") == 0 ||
			    g_ascii_strcasecmp (hader_name, "CC") == 0 ||
			    g_ascii_strcasecmp (hader_name, "BCC") == 0 ||
			    g_ascii_strcasecmp (hader_name, "Subject") == 0) {
				decoded = get_header_decoded (contents, get_default_charset (msg));
				if (decoded)
					contents = decoded;
			}

			r->value.boolean = regexec (&pattern, contents, 0, NULL, 0) == 0;
			regfree (&pattern);

			g_free (decoded);
		} else
			r->value.boolean = FALSE;

		g_object_unref (msg);
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();
	}

	return r;
}

static CamelSExpResult *
folder_search_header_full_regex (CamelSExp *sexp,
                                 gint argc,
                                 CamelSExpResult **argv,
                                 CamelFolderSearch *search)
{
	CamelSExpResult *r;
	CamelMimeMessage *msg;

	msg = get_current_message (search);

	if (msg) {
		regex_t pattern;

		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);

		if (camel_search_build_match_regex (
				&pattern,
				CAMEL_SEARCH_MATCH_REGEX |
				CAMEL_SEARCH_MATCH_ICASE |
				CAMEL_SEARCH_MATCH_NEWLINE,
				argc, argv,
				search->priv->error) == 0) {
			gchar *contents;

			contents = get_full_header (msg, get_default_charset (msg));
			r->value.boolean = regexec (&pattern, contents, 0, NULL, 0) == 0;

			g_free (contents);
			regfree (&pattern);
		} else
			r->value.boolean = FALSE;

		g_object_unref (msg);
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();
	}

	return r;
}

static CamelSExpResult *
folder_search_user_tag (CamelSExp *sexp,
                        gint argc,
                        CamelSExpResult **argv,
                        CamelFolderSearch *search)
{
	const gchar *value = NULL;
	CamelSExpResult *r;

	r (printf ("executing user-tag\n"));

	if (search->current && argc == 1)
		value = camel_message_info_user_tag (search->current, argv[0]->value.string);

	r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_STRING);
	r->value.string = g_strdup (value ? value : "");

	return r;
}

static CamelSExpResult *
folder_search_user_flag (CamelSExp *sexp,
                         gint argc,
                         CamelSExpResult **argv,
                         CamelFolderSearch *search)
{
	CamelSExpResult *r;
	gint i;

	r (printf ("executing user-flag\n"));

	/* are we inside a match-all? */
	if (search->current) {
		gint truth = FALSE;
		/* performs an OR of all words */
		for (i = 0; i < argc && !truth; i++) {
			if (argv[i]->type == CAMEL_SEXP_RES_STRING
			    && camel_message_info_user_flag (search->current, argv[i]->value.string)) {
				truth = TRUE;
				break;
			}
		}
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
		r->value.boolean = truth;
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();
	}

	return r;
}

static CamelSExpResult *
folder_search_system_flag (CamelSExp *sexp,
                           gint argc,
                           CamelSExpResult **argv,
                           CamelFolderSearch *search)
{
	CamelSExpResult *r;

	r (printf ("executing system-flag\n"));

	if (search->current) {
		gboolean truth = FALSE;

		if (argc == 1)
			truth = camel_system_flag_get (camel_message_info_flags (search->current), argv[0]->value.string);

		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
		r->value.boolean = truth;
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();
	}

	return r;
}

static CamelSExpResult *
folder_search_get_sent_date (CamelSExp *sexp,
                             gint argc,
                             CamelSExpResult **argv,
                             CamelFolderSearch *search)
{
	CamelSExpResult *r;

	r (printf ("executing get-sent-date\n"));

	/* are we inside a match-all? */
	if (search->current) {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_INT);

		r->value.number = camel_message_info_date_sent (search->current);
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();
	}

	return r;
}

static CamelSExpResult *
folder_search_get_received_date (CamelSExp *sexp,
                                 gint argc,
                                 CamelSExpResult **argv,
                                 CamelFolderSearch *search)
{
	CamelSExpResult *r;

	r (printf ("executing get-received-date\n"));

	/* are we inside a match-all? */
	if (search->current) {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_INT);

		r->value.number = camel_message_info_date_received (search->current);
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();
	}

	return r;
}

static CamelSExpResult *
folder_search_get_current_date (CamelSExp *sexp,
                                gint argc,
                                CamelSExpResult **argv,
                                CamelFolderSearch *search)
{
	CamelSExpResult *r;

	r (printf ("executing get-current-date\n"));

	r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_INT);
	r->value.number = time (NULL);
	return r;
}

static CamelSExpResult *
folder_search_get_relative_months (CamelSExp *sexp,
                                   gint argc,
                                   CamelSExpResult **argv,
                                   CamelFolderSearch *search)
{
	CamelSExpResult *r;

	r (printf ("executing get-relative-months\n"));

	if (argc != 1 || argv[0]->type != CAMEL_SEXP_RES_INT) {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
		r->value.boolean = FALSE;

		g_debug ("%s: Expecting 1 argument, an integer, but got %d arguments", G_STRFUNC, argc);
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_INT);
		r->value.number = camel_folder_search_util_add_months (time (NULL), argv[0]->value.number);
	}

	return r;
}

static CamelSExpResult *
folder_search_get_size (CamelSExp *sexp,
                        gint argc,
                        CamelSExpResult **argv,
                        CamelFolderSearch *search)
{
	CamelSExpResult *r;

	r (printf ("executing get-size\n"));

	/* are we inside a match-all? */
	if (search->current) {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_INT);
		r->value.number = camel_message_info_size (search->current) / 1024;
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();
	}

	return r;
}

static CamelSExpResult *
folder_search_uid (CamelSExp *sexp,
                   gint argc,
                   CamelSExpResult **argv,
                   CamelFolderSearch *search)
{
	CamelSExpResult *r;
	gint i;

	r (printf ("executing uid\n"));

	/* are we inside a match-all? */
	if (search->current) {
		gint truth = FALSE;
		const gchar *uid = camel_message_info_uid (search->current);

		/* performs an OR of all words */
		for (i = 0; i < argc && !truth; i++) {
			if (argv[i]->type == CAMEL_SEXP_RES_STRING
			    && !strcmp (uid, argv[i]->value.string)) {
				truth = TRUE;
				break;
			}
		}
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
		r->value.boolean = truth;
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();
		for (i = 0; i < argc; i++) {
			if (argv[i]->type == CAMEL_SEXP_RES_STRING)
				g_ptr_array_add (r->value.ptrarray, argv[i]->value.string);
		}
	}

	return r;
}

/* this is copied from Evolution's libemail-engine/e-mail-folder-utils.c */
static gchar *
mail_folder_uri_build (CamelStore *store,
                       const gchar *folder_name)
{
	const gchar *uid;
	gchar *encoded_name;
	gchar *encoded_uid;
	gchar *uri;

	g_return_val_if_fail (CAMEL_IS_STORE (store), NULL);
	g_return_val_if_fail (folder_name != NULL, NULL);

	/* Skip the leading slash, if present. */
	if (*folder_name == '/')
		folder_name++;

	uid = camel_service_get_uid (CAMEL_SERVICE (store));

	encoded_uid = camel_url_encode (uid, ":;@/");
	encoded_name = camel_url_encode (folder_name, "#");

	uri = g_strdup_printf ("folder://%s/%s", encoded_uid, encoded_name);

	g_free (encoded_uid);
	g_free (encoded_name);

	return uri;
}

static CamelSExpResult *
folder_search_message_location (CamelSExp *sexp,
                                gint argc,
                                CamelSExpResult **argv,
                                CamelFolderSearch *search)
{
	CamelSExpResult *r;
	gboolean same = FALSE;

	if (argc == 1 && argv[0]->type == CAMEL_SEXP_RES_STRING) {
		if (argv[0]->value.string && search->folder) {
			CamelStore *store;
			const gchar *name;
			gchar *uri;

			store = camel_folder_get_parent_store (search->folder);
			name = camel_folder_get_full_name (search->folder);
			uri = mail_folder_uri_build (store, name);

			same = g_str_equal (uri, argv[0]->value.string);

			g_free (uri);
		}
	}

	if (search->current) {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_BOOL);
		r->value.boolean = same ? TRUE : FALSE;
	} else {
		r = camel_sexp_result_new (sexp, CAMEL_SEXP_RES_ARRAY_PTR);
		r->value.ptrarray = g_ptr_array_new ();

		if (same) {
			/* all matches */
			gint i;
			GPtrArray *v = search->summary_set ? search->summary_set : search->summary;

			for (i = 0; i < v->len; i++) {
				gchar *uid = g_ptr_array_index (v, i);

				g_ptr_array_add (r->value.ptrarray, uid);
			}
		}
	}

	return r;
}

static void
camel_folder_search_class_init (CamelFolderSearchClass *class)
{
	GObjectClass *object_class;

	g_type_class_add_private (class, sizeof (CamelFolderSearchPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->dispose = folder_search_dispose;
	object_class->finalize = folder_search_finalize;
	object_class->constructed = folder_search_constructed;

	class->not_ = folder_search_not;
	class->match_all = folder_search_match_all;
	class->match_threads = folder_search_match_threads;
	class->body_contains = folder_search_body_contains;
	class->body_regex = folder_search_body_regex;
	class->header_contains = folder_search_header_contains;
	class->header_matches = folder_search_header_matches;
	class->header_starts_with = folder_search_header_starts_with;
	class->header_ends_with = folder_search_header_ends_with;
	class->header_exists = folder_search_header_exists;
	class->header_soundex = folder_search_header_soundex;
	class->header_regex = folder_search_header_regex;
	class->header_full_regex = folder_search_header_full_regex;
	class->user_tag = folder_search_user_tag;
	class->user_flag = folder_search_user_flag;
	class->system_flag = folder_search_system_flag;
	class->get_sent_date = folder_search_get_sent_date;
	class->get_received_date = folder_search_get_received_date;
	class->get_current_date = folder_search_get_current_date;
	class->get_relative_months = folder_search_get_relative_months;
	class->get_size = folder_search_get_size;
	class->uid = folder_search_uid;
	class->message_location = folder_search_message_location;
}

static void
camel_folder_search_init (CamelFolderSearch *search)
{
	search->priv = CAMEL_FOLDER_SEARCH_GET_PRIVATE (search);
	search->sexp = camel_sexp_new ();
}

/**
 * camel_folder_search_construct:
 * @search: a #CamelFolderSearch
 *
 * This function used to register callbacks with @search's internal
 * #CamelSExp, but this now happens during instance initialization.
 *
 * Deprecated: 3.8: The function no longer does anything.
 **/
void
camel_folder_search_construct (CamelFolderSearch *search)
{
	/* XXX constructed() method handles what used to be here. */
}

/**
 * camel_folder_search_new:
 *
 * Create a new CamelFolderSearch object.
 *
 * A CamelFolderSearch is a subclassable, extensible s-exp
 * evaluator which enforces a particular set of s-expressions.
 * Particular methods may be overriden by an implementation to
 * implement a search for any sort of backend.
 *
 * Returns: A new CamelFolderSearch widget.
 **/
CamelFolderSearch *
camel_folder_search_new (void)
{
	return g_object_new (CAMEL_TYPE_FOLDER_SEARCH, NULL);
}

/**
 * camel_folder_search_set_folder:
 * @search:
 * @folder: A folder.
 *
 * Set the folder attribute of the search.  This is currently unused, but
 * could be used to perform a slow-search when indexes and so forth are not
 * available.  Or for use by subclasses.
 **/
void
camel_folder_search_set_folder (CamelFolderSearch *search,
                                CamelFolder *folder)
{
	g_return_if_fail (CAMEL_IS_FOLDER_SEARCH (search));
	g_return_if_fail (CAMEL_IS_FOLDER (folder));

	search->folder = folder;
}

/**
 * camel_folder_search_set_summary:
 * @search:
 * @summary: (element-type CamelMessageInfo): An array of CamelMessageInfo pointers.
 *
 * Set the array of summary objects representing the span of the search.
 *
 * If this is not set, then a subclass must provide the functions
 * for searching headers and for the match-all operator.
 **/
void
camel_folder_search_set_summary (CamelFolderSearch *search,
                                 GPtrArray *summary)
{
	g_return_if_fail (CAMEL_IS_FOLDER_SEARCH (search));

	search->summary = summary;
}

/**
 * camel_folder_search_set_body_index:
 * @search:
 * @body_index:
 *
 * Set the index representing the contents of all messages
 * in this folder.  If this is not set, then the folder implementation
 * should sub-class the CamelFolderSearch and provide its own
 * body-contains function.
 **/
void
camel_folder_search_set_body_index (CamelFolderSearch *search,
                                    CamelIndex *body_index)
{
	g_return_if_fail (CAMEL_IS_FOLDER_SEARCH (search));

	if (body_index != NULL) {
		g_return_if_fail (CAMEL_IS_INDEX (body_index));
		g_object_ref (body_index);
	}

	if (search->body_index != NULL)
		g_object_unref (search->body_index);

	search->body_index = body_index;
}

static gboolean
do_search_in_memory (CamelFolder *search_in_folder,
                     const gchar *expr,
                     gchar **psql_query)
{
	/* if the expression contains any of these tokens, then perform a memory search, instead of the SQL one */
	const gchar *in_memory_tokens[] = {
		"body-contains",
		"body-regex",
		"match-threads",
		"message-location",
		"header-soundex",
		"header-regex",
		"header-full-regex",
		"header-contains",
		"header-has-words",
		"header-ends-with",
		NULL };
	gint i;

	if (search_in_folder &&
	    search_in_folder->summary &&
	    (search_in_folder->summary->flags & CAMEL_FOLDER_SUMMARY_IN_MEMORY_ONLY) != 0)
		return TRUE;

	if (!expr)
		return FALSE;

	for (i = 0; in_memory_tokens[i]; i++) {
		if (strstr (expr, in_memory_tokens[i]))
			return TRUE;
	}

	*psql_query = camel_sexp_to_sql_sexp (expr);

	/* unknown column can cause NULL sql_query, then an in-memory
	 * search is required */
	return !*psql_query;
}

/**
 * camel_folder_search_count:
 * @search:
 * @expr:
 * @cancellable: a #GCancellable
 * @error: return location for a #GError, or %NULL
 *
 * Run a search.  Search must have had Folder already set on it, and
 * it must implement summaries.
 *
 * Returns: Number of messages that match the query.
 *
 * Since: 2.26
 **/

guint32
camel_folder_search_count (CamelFolderSearch *search,
                           const gchar *expr,
                           GCancellable *cancellable,
                           GError **error)
{
	CamelSExpResult *r;
	GPtrArray *summary_set;
	gint i;
	CamelDB *cdb;
	gchar *sql_query = NULL, *tmp, *tmp1;
	GHashTable *results;
	guint32 count = 0;

	CamelFolderSearchPrivate *p;

	g_return_val_if_fail (search != NULL, 0);

	p = search->priv;

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		goto fail;

	if (!expr || !*expr)
		expr = "(match-all)";

	if (!search->folder) {
		g_warn_if_reached ();
		goto fail;
	}

	p->cancellable = cancellable;
	p->error = error;

	/* We route body-contains search and thread based search through memory and not via db. */
	if (do_search_in_memory (search->folder, expr, &sql_query)) {
		/* setup our search list only contains those we're interested in */
		search->summary = camel_folder_get_summary (search->folder);
		if (search->folder->summary)
			camel_folder_summary_prepare_fetch_all (search->folder->summary, NULL);

		summary_set = search->summary;

		/* only re-parse if the search has changed */
		if (search->last_search == NULL
		    || strcmp (search->last_search, expr)) {
			camel_sexp_input_text (search->sexp, expr, strlen (expr));
			if (camel_sexp_parse (search->sexp) == -1) {
				g_set_error (
					error, CAMEL_ERROR, CAMEL_ERROR_GENERIC,
					_("Cannot parse search expression: %s:\n%s"),
					camel_sexp_error (search->sexp), expr);
				goto fail;
			}

			g_free (search->last_search);
			search->last_search = g_strdup (expr);
		}
		r = camel_sexp_eval (search->sexp);
		if (r == NULL) {
			g_set_error (
				error, CAMEL_ERROR, CAMEL_ERROR_GENERIC,
				_("Error executing search expression: %s:\n%s"),
				camel_sexp_error (search->sexp), expr);
			goto fail;
		}

		/* now create a folder summary to return?? */
		if (r->type == CAMEL_SEXP_RES_ARRAY_PTR) {
			d (printf ("got result\n"));

			/* reorder result in summary order */
			results = g_hash_table_new (g_str_hash, g_str_equal);
			for (i = 0; i < r->value.ptrarray->len; i++) {
				d (printf ("adding match: %s\n", (gchar *) g_ptr_array_index (r->value.ptrarray, i)));
				g_hash_table_insert (results, g_ptr_array_index (r->value.ptrarray, i), GINT_TO_POINTER (1));
			}

			for (i = 0; i < summary_set->len; i++) {
				gchar *uid = g_ptr_array_index (summary_set, i);
				if (g_hash_table_lookup (results, uid))
					count++;
			}
			g_hash_table_destroy (results);
		}

		camel_sexp_result_free (search->sexp, r);

	} else {
		CamelStore *parent_store;
		const gchar *full_name;
		GError *local_error = NULL;

		full_name = camel_folder_get_full_name (search->folder);
		parent_store = camel_folder_get_parent_store (search->folder);

		/* Sync the db, so that we search the db for changes */
		camel_folder_summary_save_to_db (search->folder->summary, error);

		dd (printf ("sexp is : [%s]\n", expr));
		tmp1 = camel_db_sqlize_string (full_name);
		tmp = g_strdup_printf ("SELECT COUNT (*) FROM %s %s %s", tmp1, sql_query ? "WHERE" : "", sql_query ? sql_query : "");
		camel_db_free_sqlized_string (tmp1);
		g_free (sql_query);
		dd (printf ("Equivalent sql %s\n", tmp));

		cdb = (CamelDB *) (parent_store->cdb_r);
		camel_db_count_message_info  (cdb, tmp, &count, &local_error);
		if (local_error != NULL) {
			const gchar *message = local_error->message;
			if (strncmp (message, "no such table", 13) == 0) {
				d (g_warning ("Error during searching %s: %s\n", tmp, message));
				/* Suppress no such table */
				g_clear_error (&local_error);
			}
			g_propagate_error (error, local_error);
		}
		g_free (tmp);
	}

fail:
	/* these might be allocated by match-threads */
	if (p->threads)
		camel_folder_thread_unref (p->threads);
	if (p->threads_hash)
		g_hash_table_destroy (p->threads_hash);
	if (search->summary_set)
		g_ptr_array_free (search->summary_set, TRUE);
	if (search->summary)
		camel_folder_free_summary (search->folder, search->summary);

	p->cancellable = NULL;
	p->error = NULL;
	p->threads = NULL;
	p->threads_hash = NULL;
	search->folder = NULL;
	search->summary = NULL;
	search->summary_set = NULL;
	search->current = NULL;
	search->body_index = NULL;

	return count;
}

/**
 * camel_folder_search_search:
 * @search:
 * @expr:
 * @uids: (element-type utf8): to search against, NULL for all uid's.
 * @cancellable: a #GCancellable
 * @error: return location for a #GError, or %NULL
 *
 * Run a search.  Search must have had Folder already set on it, and
 * it must implement summaries.
 *
 * Returns: (element-type utf8) (transfer full):
 **/
GPtrArray *
camel_folder_search_search (CamelFolderSearch *search,
                            const gchar *expr,
                            GPtrArray *uids,
                            GCancellable *cancellable,
                            GError **error)
{
	CamelSExpResult *r;
	GPtrArray *matches = NULL, *summary_set;
	gint i;
	CamelDB *cdb;
	gchar *sql_query = NULL, *tmp, *tmp1;
	GHashTable *results;

	CamelFolderSearchPrivate *p;

	g_return_val_if_fail (search != NULL, NULL);

	p = search->priv;

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		goto fail;

	if (!expr || !*expr)
		expr = "(match-all)";

	if (!search->folder) {
		g_warn_if_reached ();
		goto fail;
	}

	p->cancellable = cancellable;
	p->error = error;

	/* We route body-contains / thread based search and uid search through memory and not via db. */
	if (uids || do_search_in_memory (search->folder, expr, &sql_query)) {
		/* setup our search list only contains those we're interested in */
		search->summary = camel_folder_get_summary (search->folder);

		if (uids) {
			GHashTable *uids_hash = g_hash_table_new (g_str_hash, g_str_equal);

			summary_set = search->summary_set = g_ptr_array_new ();
			for (i = 0; i < uids->len; i++)
				g_hash_table_insert (uids_hash, uids->pdata[i], uids->pdata[i]);
			for (i = 0; i < search->summary->len; i++)
				if (g_hash_table_lookup (uids_hash, search->summary->pdata[i]))
					g_ptr_array_add (search->summary_set, search->summary->pdata[i]);
			g_hash_table_destroy (uids_hash);
		} else {
			if (search->folder->summary)
				camel_folder_summary_prepare_fetch_all (search->folder->summary, NULL);
			summary_set = search->summary;
		}

		/* only re-parse if the search has changed */
		if (search->last_search == NULL
		    || strcmp (search->last_search, expr)) {
			camel_sexp_input_text (search->sexp, expr, strlen (expr));
			if (camel_sexp_parse (search->sexp) == -1) {
				g_set_error (
					error, CAMEL_ERROR, CAMEL_ERROR_GENERIC,
					_("Cannot parse search expression: %s:\n%s"),
					camel_sexp_error (search->sexp), expr);
				goto fail;
			}

			g_free (search->last_search);
			search->last_search = g_strdup (expr);
		}
		r = camel_sexp_eval (search->sexp);
		if (r == NULL) {
			g_set_error (
				error, CAMEL_ERROR, CAMEL_ERROR_GENERIC,
				_("Error executing search expression: %s:\n%s"),
				camel_sexp_error (search->sexp), expr);
			goto fail;
		}

		matches = g_ptr_array_new ();

		/* now create a folder summary to return?? */
		if (r->type == CAMEL_SEXP_RES_ARRAY_PTR) {
			d (printf ("got result\n"));

			/* reorder result in summary order */
			results = g_hash_table_new (g_str_hash, g_str_equal);
			for (i = 0; i < r->value.ptrarray->len; i++) {
				d (printf ("adding match: %s\n", (gchar *) g_ptr_array_index (r->value.ptrarray, i)));
				g_hash_table_insert (results, g_ptr_array_index (r->value.ptrarray, i), GINT_TO_POINTER (1));
			}

			for (i = 0; i < summary_set->len; i++) {
				gchar *uid = g_ptr_array_index (summary_set, i);
				if (g_hash_table_lookup (results, uid))
					g_ptr_array_add (matches, (gpointer) camel_pstring_strdup (uid));
			}
			g_hash_table_destroy (results);
		}

		camel_sexp_result_free (search->sexp, r);

	} else {
		CamelStore *parent_store;
		const gchar *full_name;
		GError *local_error = NULL;

		full_name = camel_folder_get_full_name (search->folder);
		parent_store = camel_folder_get_parent_store (search->folder);

		/* Sync the db, so that we search the db for changes */
		camel_folder_summary_save_to_db (search->folder->summary, error);

		dd (printf ("sexp is : [%s]\n", expr));
		tmp1 = camel_db_sqlize_string (full_name);
		tmp = g_strdup_printf ("SELECT uid FROM %s %s %s", tmp1, sql_query ? "WHERE":"", sql_query ? sql_query:"");
		camel_db_free_sqlized_string (tmp1);
		g_free (sql_query);
		dd (printf ("Equivalent sql %s\n", tmp));

		matches = g_ptr_array_new ();
		cdb = (CamelDB *) (parent_store->cdb_r);
		camel_db_select (
			cdb, tmp, (CamelDBSelectCB)
			read_uid_callback, matches, &local_error);
		if (local_error != NULL) {
			const gchar *message = local_error->message;
			if (strncmp (message, "no such table", 13) == 0) {
				d (g_warning ("Error during searching %s: %s\n", tmp, message));
				/* Suppress no such table */
				g_clear_error (&local_error);
			} else
				g_propagate_error (error, local_error);
		}
		g_free (tmp);

	}

fail:
	/* these might be allocated by match-threads */
	if (p->threads)
		camel_folder_thread_unref (p->threads);
	if (p->threads_hash)
		g_hash_table_destroy (p->threads_hash);
	if (search->summary_set)
		g_ptr_array_free (search->summary_set, TRUE);
	if (search->summary)
		camel_folder_free_summary (search->folder, search->summary);

	p->cancellable = NULL;
	p->error = NULL;
	p->threads = NULL;
	p->threads_hash = NULL;
	search->folder = NULL;
	search->summary = NULL;
	search->summary_set = NULL;
	search->current = NULL;
	search->body_index = NULL;

	if (error && *error) {
		camel_folder_search_free_result (search, matches);
		matches = NULL;
	}

	return matches;
}

/**
 * camel_folder_search_free_result:
 * @result: (element-type utf8):
 **/
void
camel_folder_search_free_result (CamelFolderSearch *search,
                                 GPtrArray *result)
{
	if (!result)
		return;

	g_ptr_array_foreach (result, (GFunc) camel_pstring_free, NULL);
	g_ptr_array_free (result, TRUE);
}

/**
 * camel_folder_search_util_add_months:
 * @t: Initial time
 * @months: number of months to add or subtract
 *
 * Increases time @t by the given number of months (or decreases, if
 * @months is negative).
 *
 * Returns: a new #time_t value
 *
 * Since: 3.2
 **/
time_t
camel_folder_search_util_add_months (time_t t,
                                     gint months)
{
	GDateTime *dt, *dt2;
	time_t res;

	if (!months)
		return t;

	dt = g_date_time_new_from_unix_utc (t);

	/* just for issues, to return something inaccurate, but sane */
	res = t + (60 * 60 * 24 * 30 * months);

	g_return_val_if_fail (dt != NULL, res);

	dt2 = g_date_time_add_months (dt, months);
	g_date_time_unref (dt);
	g_return_val_if_fail (dt2 != NULL, res);

	res = g_date_time_to_unix (dt2);
	g_date_time_unref (dt2);

	return res;
}
