/* GStreamer
 *
 * Copyright (C) 2015 Thibault Saunier <tsaunier@gnome.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:gsttranscoder
 * @short_description: GStreamer Transcoder API
 *
 */

#include <string.h>
#include "../gst-libs/gst/transcoding/transcoder/gsttranscoder.h"

static gchar *
ensure_uri (const gchar * location)
{
  if (gst_uri_is_valid (location))
    return g_strdup (location);
  else
    return gst_filename_to_uri (location, NULL);
}

static void
position_updated_cb (GstTranscoder * transcoder, GstClockTime pos)
{
  GstClockTime dur = -1;
  gchar status[64] = { 0, };

  g_object_get (transcoder, "duration", &dur, NULL);

  memset (status, ' ', sizeof (status) - 1);

  if (pos != -1 && dur > 0 && dur != -1) {
    gchar dstr[32], pstr[32];

    /* FIXME: pretty print in nicer format */
    g_snprintf (pstr, 32, "%" GST_TIME_FORMAT, GST_TIME_ARGS (pos));
    pstr[9] = '\0';
    g_snprintf (dstr, 32, "%" GST_TIME_FORMAT, GST_TIME_ARGS (dur));
    dstr[9] = '\0';
    g_print ("%s / %s %s\r", pstr, dstr, status);
  }
}

static GList *
get_usable_profiles (GstEncodingTarget * target)
{
  GList *tmpprof, *usable_profiles = NULL;

  for (tmpprof = (GList *) gst_encoding_target_get_profiles (target);
      tmpprof; tmpprof = tmpprof->next) {
    GstEncodingProfile *profile = tmpprof->data;
    GstElement *tmpencodebin = gst_element_factory_make ("encodebin", NULL);

    gst_encoding_profile_set_presence (profile, 1);
    if (GST_IS_ENCODING_CONTAINER_PROFILE (profile)) {
      GList *tmpsubprof;
      for (tmpsubprof = (GList *)
          gst_encoding_container_profile_get_profiles
          (GST_ENCODING_CONTAINER_PROFILE (profile)); tmpsubprof;
          tmpsubprof = tmpsubprof->next)
        gst_encoding_profile_set_presence (tmpsubprof->data, 1);
    }

    g_object_set (tmpencodebin, "profile", gst_object_ref (profile), NULL);
    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (tmpencodebin),
        GST_DEBUG_GRAPH_SHOW_ALL, gst_encoding_profile_get_name (profile));

    /* The profile could be expended */
    if (GST_BIN (tmpencodebin)->children)
      usable_profiles = g_list_prepend (usable_profiles, profile);

    gst_object_unref (tmpencodebin);
  }

  return usable_profiles;
}

int
main (int argc, char *argv[])
{
  GError *err = NULL;
  gint cpu_usage = 100;
  gboolean list_encoding_targets;
  GstTranscoder *transcoder;
  gchar *src_uri, *dest_uri;
  GOptionContext *ctx;

  GOptionEntry options[] = {
    {"cpu-usage", 'c', 0, G_OPTION_ARG_INT, &cpu_usage,
        "The CPU usage to target in the transcoding process", NULL},
    {"list-targets", 'l', G_OPTION_ARG_NONE, 0, &list_encoding_targets,
        "List all encoding targets", NULL},
    {NULL}
  };

  g_set_prgname ("gst-transcoder");

  ctx = g_option_context_new ("<source uri> <destination uri> "
      "<encoding target name>[/<encoding profile name>]");

  g_option_context_add_main_entries (ctx, options, NULL);
  g_option_context_add_group (ctx, gst_init_get_option_group ());
  if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
    g_print ("Error initializing: %s\n", GST_STR_NULL (err->message));
    g_clear_error (&err);
    g_option_context_free (ctx);
    return 1;
  }

  if (list_encoding_targets) {
    GList *tmp, *targets = gst_encoding_list_all_targets (NULL);

    for (tmp = targets; tmp; tmp = tmp->next) {
      GstEncodingTarget *target = tmp->data;
      GList *usable_profiles = get_usable_profiles (target);

      if (usable_profiles) {
        GList *tmpprof;

        g_print ("\n%s (%s): %s\n * Profiles:\n",
            gst_encoding_target_get_name (target),
            gst_encoding_target_get_category (target),
            gst_encoding_target_get_description (target));

        for (tmpprof = usable_profiles; tmpprof; tmpprof = tmpprof->next)
          g_print ("     - %s: %s",
              gst_encoding_profile_get_name (tmpprof->data),
              gst_encoding_profile_get_description (tmpprof->data));

        g_print ("\n");
        g_list_free (usable_profiles);
      }
    }

    g_list_free_full (targets, (GDestroyNotify) g_object_unref);

    return 0;
  }

  if (argc != 4) {
    g_print ("%s", g_option_context_get_help (ctx, TRUE, NULL));
    g_option_context_free (ctx);

    return -1;
  }
  g_option_context_free (ctx);

  src_uri = ensure_uri (argv[1]);
  dest_uri = ensure_uri (argv[2]);
  transcoder = gst_transcoder_new (src_uri, dest_uri, argv[3]);
  gst_transcoder_set_cpu_usage (transcoder, cpu_usage);
  g_signal_connect (transcoder, "position-updated",
      G_CALLBACK (position_updated_cb), NULL);

  g_assert (transcoder);

  gst_transcoder_run (transcoder, &err);

  if (err)
    GST_ERROR ("Badam %s", err->message);

  return 0;
}
