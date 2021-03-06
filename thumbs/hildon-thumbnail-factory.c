/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * This file is part of hildon-thumbnail package
 *
 * Copyright (C) 2005 Nokia Corporation.  All Rights reserved.
 *
 * Contact: Marius Vollmer <marius.vollmer@nokia.com>
 * Author: Philip Van Hoof <philip@codeminded.be>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include "hildon-thumbnail-factory.h"
#include "hildon-thumber-common.h"
#include "thumbnailer-client.h"
#include "thumbnailer-marshal.h"
#include "utils.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>
#include <glib/gfileutils.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>


#define THUMBNAILER_SERVICE      "org.freedesktop.thumbnailer"
#define THUMBNAILER_PATH         "/org/freedesktop/thumbnailer/Generic"
#define THUMBNAILER_INTERFACE    "org.freedesktop.thumbnailer.Generic"


typedef struct {
	gchar *uri;
	gchar *mime_type;
	guint width, height;
	HildonThumbnailFlags flags;
	HildonThumbnailFactoryFinishedCallback callback;
	gpointer user_data;
	gboolean canceled;
	GString *errors;

	guint handle_id;

} ThumbsItem;



#define THUMBS_ITEM(handle) (ThumbsItem*)(handle)
#define THUMBS_HANDLE(item) (HildonThumbnailFactoryHandle)(item)
#define HILDON_THUMBNAIL_APPLICATION "hildon-thumbnail"
#define FACTORY_ERROR g_quark_from_static_string (HILDON_THUMBNAIL_APPLICATION)

static void init (void);

static gboolean show_debug = FALSE, had_init = FALSE;
static DBusGProxy *proxy;
static DBusGConnection *connection;
static GHashTable *tasks;

typedef struct {
	gchar *file;
	guint64 mtime;
	guint64 size;
} ThumbsCacheFile;

#define TRACKER_METADATA_SERVICE	 "org.freedesktop.Tracker"
#define TRACKER_METADATA_PATH		 "/org/freedesktop/Tracker/Metadata"
#define TRACKER_METADATA_INTERFACE	 "org.freedesktop.Tracker.Metadata"

#ifdef LARGE_THUMBNAILS
	#define CHECKER 0
#else
	#ifdef NORMAL_THUMBNAILS
		#define CHECKER 1
	#else
		#define CHECKER 2
	#endif
#endif

static DBusGProxy*
get_tracker_proxy (void)
{
	static DBusGProxy *proxy_ = NULL;

	if (!proxy_) {
		GError          *error = NULL;
		DBusGConnection *connection_;

		connection_ = dbus_g_bus_get (DBUS_BUS_SESSION, &error);

		if (!error) {
			proxy_ = dbus_g_proxy_new_for_name (connection_,
			                                   TRACKER_METADATA_SERVICE,
			                                   TRACKER_METADATA_PATH,
			                                   TRACKER_METADATA_INTERFACE);
		} else {
			g_error_free (error);
		}
	}

	return proxy_;
}

/* Copied from GThumb (and/or many other projects that have copied this too)
 * Returns a copy of pixbuf mirrored and or flipped.
 * TO do a 180 degree rotations set both mirror and flipped TRUE
 * if mirror and flip are FALSE, result is a simple copy.
 */
static GdkPixbuf *
_gdk_pixbuf_copy_mirror (GdkPixbuf *src,
                         gboolean mirror,
                         gboolean flip)
{
	GdkPixbuf *dest;
	int        has_alpha;
	int        w, h, srs;
	int        drs;
	guchar    *s_pix;
	guchar    *d_pix;
	guchar    *sp;
	guchar    *dp;
	int        i, j;
	int        a;

	if (!src) return NULL;

	w = gdk_pixbuf_get_width (src);
	h = gdk_pixbuf_get_height (src);
	has_alpha = gdk_pixbuf_get_has_alpha (src);
	srs = gdk_pixbuf_get_rowstride (src);
	s_pix = gdk_pixbuf_get_pixels (src);

	dest = gdk_pixbuf_new (GDK_COLORSPACE_RGB, has_alpha, 8, w, h);
	drs = gdk_pixbuf_get_rowstride (dest);
	d_pix = gdk_pixbuf_get_pixels (dest);

	a = has_alpha ? 4 : 3;

	for (i = 0; i < h; i++) {
		sp = s_pix + (i * srs);
		if (flip)
			dp = d_pix + ((h - i - 1) * drs);
		else
			dp = d_pix + (i * drs);

		if (mirror) {
			dp += (w - 1) * a;
			for (j = 0; j < w; j++) {
				*(dp++) = *(sp++);      /* r */
				*(dp++) = *(sp++);      /* g */
				*(dp++) = *(sp++);      /* b */
				if (has_alpha) *(dp) = *(sp++); /* a */
				dp -= (a + 3);
			}
		} else {
			for (j = 0; j < w; j++) {
				*(dp++) = *(sp++);      /* r */
				*(dp++) = *(sp++);      /* g */
				*(dp++) = *(sp++);      /* b */
				if (has_alpha) *(dp++) = *(sp++);       /* a */
			}
		}
	}

	return dest;
}


GdkPixbuf* 
hildon_thumbnail_orientate (const gchar *uri, const gchar *orientation, GdkPixbuf *image)
{
	gboolean rotated = FALSE;
	GdkPixbuf *ret = image;
	GStrv values = NULL;

	if (!orientation) {
		GStrv keys;
		GError *error = NULL;

		keys = (GStrv) g_malloc0 (sizeof (gchar *) * 2);

		keys[0] = g_strdup ("Image:Orientation");
		keys[1] = NULL;

		dbus_g_proxy_call (get_tracker_proxy (),
				   "Get", &error,
				   G_TYPE_STRING, "Files",
				   G_TYPE_STRING, uri,
				   G_TYPE_STRV, keys,
				   G_TYPE_INVALID,
				   G_TYPE_STRV, &values,
				   G_TYPE_INVALID);

		if (error) {
			g_warning ("%s\n", error->message);
			g_error_free (error);
			g_strfreev (keys);
			if (values)
				g_strfreev (values);

			return image;
		}

		g_strfreev (keys);
		orientation = values[0];
	}

	if (orientation) {

		/* Mirror horizontal */
		if (g_strcmp0 (orientation, "2")) {
			ret = _gdk_pixbuf_copy_mirror (image, TRUE, FALSE);
			rotated = TRUE;
		}

		/* Rotate 180 */
		if (g_strcmp0 (orientation, "3")) {
			ret = gdk_pixbuf_rotate_simple (image, GDK_PIXBUF_ROTATE_UPSIDEDOWN);
			rotated = TRUE;
		}

		/* Mirror vertical */
		if (g_strcmp0 (orientation, "4")) {
			ret = _gdk_pixbuf_copy_mirror (image, FALSE, TRUE);
			rotated = TRUE;
		}

		/* Mirror horizontal and rotate 270 CW */
		if (g_strcmp0 (orientation, "5")) {
			GdkPixbuf *temp = _gdk_pixbuf_copy_mirror (image, TRUE, FALSE);
			ret = gdk_pixbuf_rotate_simple (temp, GDK_PIXBUF_ROTATE_CLOCKWISE);
			g_object_unref (temp);
			rotated = TRUE;
		}

		/* Rotate 90 CW  */
		if (g_strcmp0 (orientation, "6")) {
			ret = gdk_pixbuf_rotate_simple (image, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
			rotated = TRUE;
		}

		/* Mirror horizontal and rotate 90 CW  */
		
		if (g_strcmp0 (orientation, "7")) {
			GdkPixbuf *temp = _gdk_pixbuf_copy_mirror (image, TRUE, FALSE);
			ret = gdk_pixbuf_rotate_simple (temp, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
			g_object_unref (temp);
			rotated = TRUE;
		}

		/* Rotate 270 CW  */
		if (g_strcmp0 (orientation, "8")) {
			ret = gdk_pixbuf_rotate_simple (image, GDK_PIXBUF_ROTATE_CLOCKWISE);
			rotated = TRUE;
		}

	}

	if (values)
		g_strfreev (values);

	if (!rotated)
		g_object_ref (image);

	return ret;
}

static void thumb_item_free(ThumbsItem* item)
{
	g_free(item->uri);
	g_free(item->mime_type);
	if (item->errors)
		g_string_free (item->errors, TRUE);
	g_free(item);
}

static void
create_pixbuf_and_callback (ThumbsItem *item, gchar *large, gchar *normal, gchar *cropped, gboolean uris_as_paths)
{
		GFile *filei = NULL;
		GInputStream *stream = NULL;
		GdkPixbuf *pixbuf = NULL;
		gchar *path;
		GError *error = NULL;

		/* Determine the exact type of thumbnail being requested */

		if (item->flags & HILDON_THUMBNAIL_FLAG_CROP) {
			path = g_strdup (cropped);
		}
		else if (item->width > 128 || item->height > 128) {
			path = g_strdup (large);
			path = g_strdup (normal);
		} 
		else {
			path = g_strdup (normal);
		}

		/* Open the original thumbnail as a stream */
		if (uris_as_paths)
			filei = g_file_new_for_uri (path);
		else
			filei = g_file_new_for_path (path);

		stream = G_INPUT_STREAM (g_file_read (filei, NULL, &error));
		g_free (path);

		if (error)
			goto error_handler;

		/* Read the stream as a pixbuf at the requested exact scale */
		pixbuf = my_gdk_pixbuf_new_from_stream_at_scale (stream,
			item->width, item->height, TRUE, 
			NULL, &error);

		error_handler:

		/* Callback user function, passing the pixbuf and error */

		if (item->errors) {
			if (!error)
				g_set_error (&error, FACTORY_ERROR, 0, "%s", item->errors->str);
			else {
				g_string_append (item->errors, " - ");
				g_string_append (item->errors, error->message);
				g_clear_error (&error);
				g_set_error (&error, FACTORY_ERROR, 0, "%s", item->errors->str);
			}
		}

		if (item->callback)
			item->callback (item, item->user_data, pixbuf, error);

		/* Cleanup */
		if (filei)
			g_object_unref (filei);

		if (stream) {
			g_input_stream_close (G_INPUT_STREAM (stream), NULL, NULL);
			g_object_unref (stream);
		}

		if (error)
			g_error_free (error);

		if (pixbuf)
			gdk_pixbuf_unref (pixbuf);
}

static void
on_task_error (DBusGProxy *proxy_,
		  guint       handle,
		  GStrv       failed_uris,
		  gint        error_code,
		  gchar      *error_message,
		  gpointer    user_data)
{
	gchar *key = g_strdup_printf ("%d", handle);
	ThumbsItem *item = g_hash_table_lookup (tasks, key);

	if (item) {
		if (!item->errors) {
			item->errors = g_string_new (error_message);
		} else {
			g_string_append (item->errors, " - ");
			g_string_append (item->errors, error_message);
		}
	}

	g_free (key);
}

static void
on_task_finished (DBusGProxy *proxy_,
		  guint       handle,
		  gpointer    user_data)
{
	gchar *key = g_strdup_printf ("%d", handle);
	ThumbsItem *item = g_hash_table_lookup (tasks, key);

	if (item) {
			gchar *large = NULL, *normal = NULL, *cropped = NULL;
			gchar *path;

			/* Get the large small and cropped path for the original
			 * URI */
			
			hildon_thumbnail_util_get_thumb_paths (item->uri, &large, 
								&normal, &cropped,
								NULL, NULL, NULL,
								FALSE);

			if (item->flags & HILDON_THUMBNAIL_FLAG_CROP) {
				path = cropped;
			} 
			else if (item->width > 128 || item->height > 128) {
				path = large;
			} 
			else {
				path = normal;
			}

			if (!g_file_test (path, G_FILE_TEST_EXISTS)) {

				g_free (large); large = NULL;
				g_free (normal); normal = NULL;
				g_free (cropped); cropped = NULL;

				hildon_thumbnail_util_get_thumb_paths (item->uri, &large, 
								       &normal, &cropped,
								       NULL, NULL, NULL,
								       TRUE);
			}

			create_pixbuf_and_callback (item, large, normal, cropped, FALSE);

			g_free (cropped);
			g_free (normal);
			g_free (large);

			/* Remove the key from the hash, which means that we declare it 
			 * handled. */
			g_hash_table_remove (tasks, key);
	}

	g_free (key);

}

static void 
read_cache_dir(gchar *path, GPtrArray *files)
{
	GDir *dir;
	const gchar *file;

	dir = g_dir_open(path, 0, NULL);

	if(dir) {
		while((file = g_dir_read_name(dir)) != NULL) {
			gchar *file_path;
			ThumbsCacheFile *item;
			GFile *filei;
			GFileInfo *info;
			GError *error = NULL;
			guint64 mtime, size;

			file_path = g_build_filename (path, file, NULL);

			if(file[0] == '.' || !g_file_test(file_path, G_FILE_TEST_IS_REGULAR)) {
				g_free (file_path);
				continue;
			}

			filei = g_file_new_for_path (file_path);

			info = g_file_query_info (filei, G_FILE_ATTRIBUTE_TIME_MODIFIED ","
						  G_FILE_ATTRIBUTE_STANDARD_SIZE,
						  G_FILE_QUERY_INFO_NONE,
						  NULL, &error);

			if (error) {
				g_error_free (error);
				g_object_unref (filei);
				g_free (file_path);
				continue;
			}

			mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
			size = g_file_info_get_size (info);

			g_object_unref (filei);
			g_object_unref (info);

			item = g_new(ThumbsCacheFile, 1);
			item->file = file_path;
			item->mtime = mtime;
			item->size = size;

			g_ptr_array_add(files, item);
		}

		g_dir_close(dir);
	}
}

static void
cache_file_free(ThumbsCacheFile *item)
{
	g_free (item->file);
	g_free (item);
}

static gint 
cache_file_compare(gconstpointer a, gconstpointer b)
{
	ThumbsCacheFile *f1 = *(ThumbsCacheFile**)a,
			        *f2 = *(ThumbsCacheFile**)b;

	/* Sort in descending order */
	if(f2->mtime == f1->mtime) {
		return 0;
	} else if(f2->mtime < f1->mtime) {
		return -1;
	} else {
		return 1;
	}
}



void 
hildon_thumbnail_factory_clean_cache(gint max_size, time_t min_mtime)
{
	GPtrArray *files;
	int i, size = 0;
	gchar *large_dir = g_build_filename (g_get_home_dir (), ".thumbnails", "large", NULL);
	gchar *normal_dir = g_build_filename (g_get_home_dir (), ".thumbnails", "normal", NULL);
	gchar *cropped_dir = g_build_filename (g_get_home_dir (), ".thumbnails", "cropped", NULL);
	gchar *fail_dir = g_build_filename (g_get_home_dir (), ".thumbnails", "fail", NULL);

	init ();

	files = g_ptr_array_new();

	read_cache_dir (fail_dir, files);
	read_cache_dir (large_dir, files);
	read_cache_dir (normal_dir, files);
	read_cache_dir (cropped_dir, files);

	g_ptr_array_sort (files, cache_file_compare);

	for(i = 0; i < files->len; i++) {
		ThumbsCacheFile *item = g_ptr_array_index (files, i);

		size += item->size;
		if ((max_size >= 0 && size >= max_size) || item->mtime < min_mtime) {
			unlink (item->file);
		}
	}

	g_ptr_array_foreach (files, (GFunc)cache_file_free, NULL);
	g_ptr_array_free (files, TRUE);

	g_free (fail_dir);
	g_free (normal_dir);
	g_free (large_dir);
	g_free (cropped_dir);
}

static gboolean waiting_for_cb = FALSE;

static void 
on_got_handle (DBusGProxy *proxy_, guint OUT_handle, GError *error, gpointer userdata)
{
	ThumbsItem *item = userdata;
	gchar *key = g_strdup_printf ("%d", OUT_handle);
	item->handle_id = OUT_handle;
	if (!error) {
		/* Register the item as being handled */
		g_hash_table_replace (tasks, key, item);
	} else {
		item->callback (item, item->user_data, NULL, error);
		g_free (key);
	}
	waiting_for_cb = FALSE;

}

typedef struct {
	gchar *large, *normal, *cropped;
	gchar *local_large, *local_normal, *local_cropped;
	ThumbsItem *item;
} ThumbsItemAndPaths;

static void
free_thumbsitem_and_paths (ThumbsItemAndPaths *info) 
{
	g_free (info->large);
	g_free (info->normal);
	g_free (info->cropped);
	g_free (info->local_large);
	g_free (info->local_normal);
	g_free (info->local_cropped);
	thumb_item_free (info->item);
	g_slice_free (ThumbsItemAndPaths, info);
}

static gboolean
have_all_cb (gpointer user_data)
{
	ThumbsItemAndPaths *info = user_data;
	ThumbsItem *item = info->item;
	gchar *large, *normal, *cropped;
	gboolean uris = FALSE;
	GFile *local;

	local = g_file_new_for_uri (info->local_large);

	if (g_file_query_exists (local, NULL)) {
		large = info->local_large;
		uris = TRUE;
	} else 
		large = info->large;

	g_object_unref (local);


	local = g_file_new_for_uri (info->local_normal);

	if (g_file_query_exists (local, NULL)) {
		normal = info->local_normal;
		uris = TRUE;
	} else 
		normal = info->local_normal;

	g_object_unref (local);

	local = g_file_new_for_uri (info->local_cropped);

	if (g_file_query_exists (local, NULL)) {
		cropped = info->local_cropped;
		uris = TRUE;
	} else 
		cropped = info->cropped;

	g_object_unref (local);

	create_pixbuf_and_callback (item, large, normal, cropped, uris);

	return FALSE;
}

static gboolean
new_enough (const gchar *orig_uri, const gchar *thumb_path)
{
	GFileInfo *info1, *info2;
	GFile *file1;
	GFile *file2;
	gboolean retval = TRUE;

	if (!g_file_test (thumb_path, G_FILE_TEST_EXISTS))
		return FALSE;

	file1 = g_file_new_for_uri (orig_uri);
	file2 = g_file_new_for_path (thumb_path);

	info1 = g_file_query_info (file1, 
				   G_FILE_ATTRIBUTE_TIME_MODIFIED,
				   G_FILE_QUERY_INFO_NONE, NULL, NULL);

	info2 = g_file_query_info (file2, 
				   G_FILE_ATTRIBUTE_TIME_MODIFIED,
				   G_FILE_QUERY_INFO_NONE, NULL, NULL);

	if (info1 && info2) {
		guint64 fmtime1, fmtime2;

		fmtime1 = g_file_info_get_attribute_uint64 (info1, 
							   G_FILE_ATTRIBUTE_TIME_MODIFIED);
		fmtime2 = g_file_info_get_attribute_uint64 (info2, 
							   G_FILE_ATTRIBUTE_TIME_MODIFIED);

		if (fmtime1 != fmtime2) {
			retval = FALSE;
		}
	}

	if (info1)
		g_object_unref (info1);

	if (info2)
		g_object_unref (info2);

	g_object_unref (file1);
	g_object_unref (file2);

	return retval;
}


HildonThumbnailFactoryHandle hildon_thumbnail_factory_load_custom(
				const gchar *uri, const gchar *mime_type,
				guint width, guint height,
				HildonThumbnailFactoryFinishedCallback callback,
				gpointer user_data, 
				HildonThumbnailFlags flags, ...)
{
	gchar *large = NULL, *normal = NULL, *cropped = NULL;
	gchar *local_large = NULL, *local_normal = NULL, *local_cropped = NULL;
	ThumbsItem *item;
	GStrv uris;
	GStrv mimes;
	gboolean have_all = FALSE;
	guint y = 0;
	gboolean do_cropped = TRUE;

#ifdef LARGE_THUMBNAILS
	do_cropped = (flags & HILDON_THUMBNAIL_FLAG_CROP);
#endif

#ifdef NORMAL_THUMBNAILS
	do_cropped = (flags & HILDON_THUMBNAIL_FLAG_CROP);
#endif

	if (do_cropped)
		flags |= HILDON_THUMBNAIL_FLAG_CROP;
	else
		flags &= ~HILDON_THUMBNAIL_FLAG_CROP;

	g_return_val_if_fail(uri != NULL && mime_type != NULL && callback != NULL,
			     NULL);

	g_debug ("Thumbnail request for %s at %dx%d %s\n",
		 uri, width, height, cropped?"CROPPED":"NON-CROPPED");

	for (y = 0; y < 2; y++) {

		g_free (normal); normal = NULL;
		g_free (large); large = NULL;
		g_free (cropped); cropped = NULL;
		g_free (local_normal); local_normal = NULL;
		g_free (local_large); local_large = NULL;
		g_free (local_cropped); local_cropped = NULL;

		hildon_thumbnail_util_get_thumb_paths (uri, &large, &normal, 
						       &cropped, &local_large, 
						       &local_normal, &local_cropped,
						       (y == 0));

		if (flags & HILDON_THUMBNAIL_FLAG_CROP) {
			if (new_enough (uri, cropped))
				break;
		} 
		else if (width > 128 || height > 128) {
			if (new_enough (uri, large))
				break;
		} 
		else {
			if (new_enough (uri, normal))
				break;
		}

	}

	if (flags & HILDON_THUMBNAIL_FLAG_RECREATE) {
		g_unlink (large);
		g_unlink (normal);
		g_unlink (cropped);
	} else {
		gchar *path, *luri;
		GFile *local;

		if (flags & HILDON_THUMBNAIL_FLAG_CROP) {
			path = cropped;
			luri = local_cropped;
		} 
		else if (width > 128) {
			path = large;
			luri = local_large;
		} 
		else {
			path = normal;
			luri = local_normal;
		}
		local = g_file_new_for_uri (luri);
		have_all = (new_enough (uri, path) || g_file_query_exists (local, NULL));
		g_object_unref (local);
	}

	item = g_new (ThumbsItem, 1);

	item->uri = g_strdup (uri);
	if (mime_type)
		item->mime_type = g_strdup (mime_type);
	else
		item->mime_type = NULL;
	item->width = width;
	item->height = height;
	item->callback = callback;
	item->user_data = user_data;
	item->flags = flags;
	item->canceled = FALSE;
	item->handle_id = 0;
	item->errors = NULL;

	if (have_all) {
		ThumbsItemAndPaths *info = g_slice_new0 (ThumbsItemAndPaths);

		info->item = item;
		info->normal = normal;
		info->large = large;
		info->cropped = cropped;

		info->local_normal = local_normal;
		info->local_large = local_large;
		info->local_cropped = local_cropped;

		g_idle_add_full (G_PRIORITY_DEFAULT, have_all_cb, info,
				 (GDestroyNotify) free_thumbsitem_and_paths);

		return item;
	}

	g_free (large);
	g_free (normal);
	g_free (cropped);

	g_free (local_large);
	g_free (local_normal);
	g_free (local_cropped);

	if (!have_all) {

		init ();
		uris = (GStrv) g_malloc0 (sizeof (gchar *) * 2);
		uris[0] = g_strdup (uri);

		if (mime_type) {
			mimes = (GStrv) g_malloc0 (sizeof (gchar *) * 2);
			mimes[0] = g_strdup (mime_type);
		} else
			mimes = NULL;

		waiting_for_cb = TRUE;
		org_freedesktop_thumbnailer_Generic_queue_async (proxy, 
								 (const char **) uris, 
								 (const char **) mimes,
								 0, 
								 on_got_handle, item);

		g_strfreev (uris);
		if (mimes)
			g_strfreev (mimes);
	}

	return THUMBS_HANDLE (item);
}

HildonThumbnailFactoryHandle hildon_thumbnail_factory_load(
				const gchar *uri, const gchar *mime_type,
				guint width, guint height,
				HildonThumbnailFactoryFinishedCallback callback,
				gpointer user_data)
{
	return hildon_thumbnail_factory_load_custom(uri, mime_type, width, height,
		callback, user_data, HILDON_THUMBNAIL_FLAG_CROP, -1);
}

static void 
on_cancelled (DBusGProxy *proxy_, GError *error, gpointer userdata)
{
}

void hildon_thumbnail_factory_cancel(HildonThumbnailFactoryHandle handle)
{
	ThumbsItem *item = THUMBS_ITEM (handle);
	gchar *key;

	init();

	if (item->handle_id == 0)
		return;

	key = g_strdup_printf ("%d", item->handle_id);
	/* Unregister the item */
	g_hash_table_remove (tasks, key);
	g_free (key);

	/* We don't do real canceling, we just do unqueing */
	org_freedesktop_thumbnailer_Generic_unqueue_async (proxy, item->handle_id, 
							   on_cancelled, item);

}

void hildon_thumbnail_factory_wait()
{
	init();

	while (waiting_for_cb)
		g_main_context_iteration (NULL, FALSE);

	while(g_hash_table_size (tasks) != 0) {
		g_main_context_iteration(NULL, FALSE);
	}
}

static void file_opp_reply  (DBusGProxy *proxy_, GError *error, gpointer userdata)
{
}

void hildon_thumbnail_factory_move(const gchar *src_uri, const gchar *dest_uri)
{
	GStrv in, out;

	g_return_if_fail(src_uri && dest_uri && strcmp(src_uri, dest_uri));

	init();

	in = (GStrv) g_malloc0 (sizeof (gchar *) * 2);
	out = (GStrv) g_malloc0 (sizeof (gchar *) * 2);

	in[0] = g_strdup (src_uri);
	out[0] = g_strdup (dest_uri);

	org_freedesktop_thumbnailer_Generic_move_async (proxy, 
							(const char **) in,
							(const char **) out, 
							file_opp_reply,
							NULL);

	g_strfreev (in);
	g_strfreev (out);
}

void hildon_thumbnail_factory_copy(const gchar *src_uri, const gchar *dest_uri)
{
	GStrv in, out;

	g_return_if_fail(src_uri && dest_uri && strcmp(src_uri, dest_uri));

	init();

	in = (GStrv) g_malloc0 (sizeof (gchar *) * 2);
	out = (GStrv) g_malloc0 (sizeof (gchar *) * 2);

	in[0] = g_strdup (src_uri);
	out[0] = g_strdup (dest_uri);

	org_freedesktop_thumbnailer_Generic_copy_async (proxy, 
							(const char **) in,
							(const char **) out, 
							file_opp_reply,
							NULL);

	g_strfreev (in);
	g_strfreev (out);

}

void hildon_thumbnail_factory_remove(const gchar *uri)
{
	GStrv in;

	g_return_if_fail(uri);

	init();

	in = (GStrv) g_malloc0 (sizeof (gchar *) * 2);

	in[0] = g_strdup (uri);

	org_freedesktop_thumbnailer_Generic_delete_async (proxy, 
							(const char **) in, 
							file_opp_reply,
							NULL);

	g_strfreev (in);
}

void hildon_thumbnail_factory_move_front(HildonThumbnailFactoryHandle handle)
{
	init ();

	//g_warning ("hildon_thumbnail_factory_move_front is deprecated");
}

void hildon_thumbnail_factory_move_front_all_from(HildonThumbnailFactoryHandle handle)
{
	init ();

	//g_warning ("hildon_thumbnail_factory_move_front_all_from is deprecated");
}

void hildon_thumbnail_factory_set_debug(gboolean debug)
{
	init ();

	show_debug = debug;
}



static void init (void) {
	if (!had_init) {
		GError *error = NULL;

		tasks = g_hash_table_new_full (g_str_hash, g_str_equal,
					       (GDestroyNotify) g_free,
					       (GDestroyNotify) thumb_item_free);

		connection = dbus_g_bus_get (DBUS_BUS_SESSION, &error);

		if (error) {
			g_critical ("Can't initialize HildonThumbnailer: %s\n", error->message);
			g_error_free (error);
			g_hash_table_unref (tasks);
			tasks = NULL;
			connection = NULL;
			had_init = FALSE;
			abort ();
			return;
		}

		proxy = dbus_g_proxy_new_for_name (connection, 
					   THUMBNAILER_SERVICE,
					   THUMBNAILER_PATH,
					   THUMBNAILER_INTERFACE);

		dbus_g_proxy_add_signal (proxy, "Finished", 
					G_TYPE_UINT, G_TYPE_INVALID);

		dbus_g_object_register_marshaller (thumbnailer_marshal_VOID__UINT_BOXED_INT_STRING,
					G_TYPE_NONE,
					G_TYPE_UINT, 
					G_TYPE_STRV,
					G_TYPE_INT,
					G_TYPE_STRING,
					G_TYPE_INVALID);
	
		dbus_g_proxy_add_signal (proxy, "Error", 
					G_TYPE_UINT, 
					G_TYPE_STRV,
					G_TYPE_INT,
					G_TYPE_STRING,
					G_TYPE_INVALID);

		dbus_g_proxy_connect_signal (proxy, "Finished",
				     G_CALLBACK (on_task_finished),
				     NULL,
				     NULL);

		dbus_g_proxy_connect_signal (proxy, "Error",
				     G_CALLBACK (on_task_error),
				     NULL,
				     NULL);

	}

	had_init = TRUE;
}

int 
hildon_thumber_main (int *argc_p, char ***argv_p, HildonThumberCreateThumb create_thumb)
{
	g_warning ("hildon_thumber_main is deprecated\n");
	return -1;
}

GdkPixbuf* 
hildon_thumber_create_empty_pixbuf (void)
{
	return gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 1, 1);
}

GQuark 
hildon_thumbnail_error_quark (void)
{
	return FACTORY_ERROR;
}

gboolean
hildon_thumbnail_is_cached (const gchar *uri, guint width, guint height, gboolean is_cropped)
{
	gboolean retval;
	gchar *urif;
	GFile *file;

	urif = hildon_thumbnail_get_uri (uri, width, height, is_cropped);
	file = g_file_new_for_uri (urif);

	retval = g_file_query_exists (file, NULL);

        /* Check if the cached thumbnail is obsolete */
        if (retval) {
          GFileInfo *info1, *info2;
          GFile *file1;
          GFile *file2;

          guint64 fmtime1, fmtime2;
          file1 = g_file_new_for_uri (uri);
          file2 = g_file_new_for_uri (urif);

          info1 = g_file_query_info (file1, 
              G_FILE_ATTRIBUTE_TIME_MODIFIED,
              G_FILE_QUERY_INFO_NONE, NULL, NULL);

          info2 = g_file_query_info (file2, 
              G_FILE_ATTRIBUTE_TIME_MODIFIED,
              G_FILE_QUERY_INFO_NONE, NULL, NULL);

          if (info1 != NULL && info2 != NULL) {
            fmtime1 = g_file_info_get_attribute_uint64 (info1, 
                G_FILE_ATTRIBUTE_TIME_MODIFIED);
            fmtime2 = g_file_info_get_attribute_uint64 (info2, 
                G_FILE_ATTRIBUTE_TIME_MODIFIED);

            if (fmtime1 != fmtime2) {
                retval = FALSE;
            }
          }
          g_object_unref (file1);
          g_object_unref (file2);

          if (info1)
            g_object_unref (info1);
          if (info2)
            g_object_unref (info2);
        }

	g_free (urif);

	g_object_unref (file);

	return retval;
}

gchar *
hildon_thumbnail_get_uri (const gchar *uri, guint width, guint height, gboolean is_cropped)
{
	gchar *large, *normal, *cropped, *local_large, *local_normal, *local_cropped;
	gchar *path;

	hildon_thumbnail_util_get_thumb_paths (uri, &large, &normal, 
						&cropped, &local_large, 
						&local_normal, &local_cropped, FALSE);

	if (is_cropped) {

		GFile *fcropped = g_file_new_for_uri (local_cropped);
		if (g_file_query_exists (fcropped, NULL))
			path = g_strdup (local_cropped);
		else 
			path = g_filename_to_uri(cropped, NULL, NULL);
		g_object_unref (fcropped);

	} 
#ifdef LARGE_THUMBNAILS
	else if (width <= 128 || height <= 128) {

		GFile *fnormal = g_file_new_for_uri (local_normal);
		if (g_file_query_exists (fnormal, NULL))
			path = g_filename_to_uri (local_normal, NULL, NULL);
		else 
			path = g_filename_to_uri (normal, NULL, NULL);
		g_object_unref (fnormal);
	} 
#endif
	else {
		GFile *flarge = g_file_new_for_uri (local_large);
		if (g_file_query_exists (flarge, NULL))
			path = g_strdup (local_large);
		else 
			path = g_filename_to_uri (large, NULL, NULL);
		g_object_unref (flarge);
	}

	g_free (large);
	g_free (normal);
	g_free (cropped);
	g_free (local_large);
	g_free (local_normal);
	g_free (local_cropped);

	return path;
}

