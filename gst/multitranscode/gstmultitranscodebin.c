/* GStreamer
 * Copyright (C) 2018 Seungha Yang <pudding8757@gmail.com>
 *
 * gstmultitranscodebin.c:
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
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gstmultitranscoding.h"
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#include <glib/gstdio.h>

#include <gst/pbutils/missing-plugins.h>

GST_DEBUG_CATEGORY_STATIC (gst_multi_transcodebin_debug);
#define GST_CAT_DEFAULT gst_multi_transcodebin_debug

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate request_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);

typedef struct _GstTransChain GstTransChain;
typedef struct _GstTransChild GstTransChild;
typedef struct _GstTransInput GstTransInput;

typedef struct
{
  GstBin parent;

  /* Protect input */
  GMutex input_lock;
  /* Main input */
  GstTransInput *main_input;
  /* Supplementary input (request sink pads) */
  GList *other_inputs;
  /* counter for input */
  guint32 input_counter;

  /* List of each encoding path (i.e., GstTransChild) */
  GList *childs;
  /* Per decodebin output pad (i.e., GstTransChain) */
  GList *transchain;

  /* properties */
  gchar *root_dir;
  gint target_duration;
  GstEncodingTarget *encoding_target;

  guint num_child;
} GstMultiTranscodeBin;

typedef struct
{
  GstBinClass parent;

} GstMultiTranscodeBinClass;

struct _GstTransChain
{
  GstPad *sinkpad;              /* has refcount */
  GstElement *tee;

  GstMultiTranscodeBin *parent; /* weak reference */
};

struct _GstTransChild
{
  GstElement *bin;
  GstPad *video_sink;
  GstPad *audio_sink;

  GstElement *muxer;
  GstElement *sink;
};

struct _GstTransInput
{
  GstMultiTranscodeBin *tbin;   /* weak reference */

  gboolean is_main;

  GstElement *decodebin;        /* has refcount */
  GstPad *decodebin_sink;
  GstPad *ghost_sink;

  gulong pad_added_sigid;
  gulong no_more_pads_sigid;
};

static GstBinClass *parent_class;

#define GST_TYPE_MULTI_TRANSCODE_BIN (gst_multi_transcode_bin_get_type ())
#define GST_MULTI_TRANSCODE_BIN(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_MULTI_TRANSCODE_BIN, GstMultiTranscodeBin))
#define GST_MULTI_TRANSCODE_BIN_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST ((klass), GST_MULTI_TRANSCODE_BIN_TYPE, GstMultiTranscodeBinClass))
#define GST_IS_TRANSCODE_BIN(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_MULTI_TRANSCODE_BIN_TYPE))
#define GST_IS_TRANSCODE_BIN_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_MULTI_TRANSCODE_BIN_TYPE))
#define GST_MULTI_TRANSCODE_BIN_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_MULTI_TRANSCODE_BIN_TYPE, GstMultiTranscodeBinClass))

#define INPUT_LOCK(tbin) G_STMT_START {   \
    GST_LOG_OBJECT (tbin,                 \
        "input locking from thread %p",   \
        g_thread_self ());                \
    g_mutex_lock (&tbin->input_lock);     \
    GST_LOG_OBJECT (tbin,                 \
        "input locked from thread %p",    \
        g_thread_self ());                \
  } G_STMT_END

#define INPUT_UNLOCK(tbin) G_STMT_START { \
    GST_LOG_OBJECT (tbin,                 \
        "input unlocking from thread %p", \
        g_thread_self ());                \
    g_mutex_unlock (&tbin->input_lock);   \
  } G_STMT_END

#define X264_PRESET_NAME "transcodebin_x264_preset"

static void gst_multi_transcode_bin_class_init (GstMultiTranscodeBinClass *
    klass);
static void gst_multi_transcode_bin_init (GstMultiTranscodeBin * self);
static GstPad *gst_multi_transcode_bin_request_new_pad (GstElement * element,
    GstPadTemplate * temp, const gchar * name, const GstCaps * caps);
static GstStateChangeReturn gst_multi_transcode_bin_change_state (GstElement *
    element, GstStateChange transition);

static void gst_multi_transcode_bin_dispose (GObject * object);
static void gst_multi_transcode_bin_finalize (GObject * object);
static void gst_multi_transcode_bin_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_multi_transcode_bin_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);

static void free_input (GstMultiTranscodeBin * self, GstTransInput * input);
static void free_input_async (GstMultiTranscodeBin * self,
    GstTransInput * input);
static GstTransInput *create_new_input (GstMultiTranscodeBin * self,
    gboolean main);
static void pad_added_cb (GstElement * decodebin, GstPad * pad,
    GstMultiTranscodeBin * self);
static void no_more_pads_cb (GstElement * decodebin,
    GstMultiTranscodeBin * self);
static gboolean configure_video_encoding (GstMultiTranscodeBin * self,
    GstTransChild * child, GstEncodingVideoProfile * profile);
static gboolean configure_audio_encoding (GstMultiTranscodeBin * self,
    GstTransChild * child, GstEncodingAudioProfile * profile);


static GType
gst_multi_transcode_bin_get_type (void)
{
  static GType gst_multi_transcode_bin_type = 0;

  if (!gst_multi_transcode_bin_type) {
    static const GTypeInfo gst_multi_transcode_bin_info = {
      sizeof (GstMultiTranscodeBinClass),
      NULL,
      NULL,
      (GClassInitFunc) gst_multi_transcode_bin_class_init,
      NULL,
      NULL,
      sizeof (GstMultiTranscodeBin),
      0,
      (GInstanceInitFunc) gst_multi_transcode_bin_init,
      NULL
    };

    gst_multi_transcode_bin_type =
        g_type_register_static (GST_TYPE_BIN, "GstMultiTranscodeBin",
        &gst_multi_transcode_bin_info, 0);
  }

  return gst_multi_transcode_bin_type;
}

/* signals */
enum
{
  SIGNAL_DECODEBIN_SETUP,
  LAST_SIGNAL
};

#define DEFAULT_ROOT_DIR NULL
#define DEFAULT_ENABLE_RECORD FALSE
#define DEFAULT_TARGET_DURATION 3

/* Properties */
enum
{
  PROP_0,
  PROP_ROOT_DIR,
  PROP_TARGET_DURATION,
  PROP_ENCODING_TARGET,
  LAST_PROP
};

static guint gst_multi_transcode_bin_signals[LAST_SIGNAL] = { 0 };

static void
gst_multi_transcode_bin_class_init (GstMultiTranscodeBinClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_klass;

  object_class->dispose = gst_multi_transcode_bin_dispose;
  object_class->finalize = gst_multi_transcode_bin_finalize;
  object_class->get_property = gst_multi_transcode_bin_get_property;
  object_class->set_property = gst_multi_transcode_bin_set_property;

  parent_class = g_type_class_peek_parent (klass);

  gstelement_klass = (GstElementClass *) klass;
  gstelement_klass->change_state =
      GST_DEBUG_FUNCPTR (gst_multi_transcode_bin_change_state);
  gstelement_klass->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_multi_transcode_bin_request_new_pad);

  gst_element_class_add_pad_template (gstelement_klass,
      gst_static_pad_template_get (&sink_template));
  gst_element_class_add_pad_template (gstelement_klass,
      gst_static_pad_template_get (&request_sink_template));

  /**
   * GstMultiTranscodeBin::decodebin-setup:
   * @multitranscodebin: a #GstMultiTranscodeBin
   * @decodebin: decodebin
   *
   * This signal is emitted after the decodebin fires no-more-pads signal.
   *
   * This signal is usually emitted from the context of a GStreamer streaming
   * thread.
   */
  gst_multi_transcode_bin_signals[SIGNAL_DECODEBIN_SETUP] =
      g_signal_new ("decodebin-setup", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      g_cclosure_marshal_generic, G_TYPE_NONE, 1, GST_TYPE_ELEMENT);

  g_object_class_install_property (object_class, PROP_ROOT_DIR,
      g_param_spec_string ("root-dir", "Root Directory",
          "Root Directory of files to write", DEFAULT_ROOT_DIR,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_TARGET_DURATION,
      g_param_spec_uint ("target-duration", "Target duration",
          "The target duration in seconds of a segment/file. "
          "(0 - disabled, useful for management of segment duration by the "
          "streaming server)",
          0, G_MAXUINT, DEFAULT_TARGET_DURATION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_ENCODING_TARGET,
      g_param_spec_object ("encoding-target", "Encoding Target",
          "The GstEncodingTarget to use", GST_TYPE_ENCODING_TARGET,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (gstelement_klass, "Transcodebin",
      "Generic/Bin/Sink",
      "Convenience bin for transcoding",
      "Seungha Yang <pudding8757@gmail.com>");
}

#define DEFAULT_X264_720P_PRESET_NAME "multitranscodebin-x264-preset-720p"
#define DEFAULT_X264_480P_PRESET_NAME "multitranscodebin-x264-preset-480p"
#define DEFAULT_X264_240P_PRESET_NAME "multitranscodebin-x264-preset-240p"

static void
_create_default_x264_preset (const gchar * preset_name, guint bitrate)
{
  GstPreset *preset = GST_PRESET (gst_element_factory_make ("x264enc", NULL));
  g_object_set (G_OBJECT (preset), "option-string",
      "threads=4:bframes=0:keyint=30:min-keyint=30:scenecut=0:cabac=1",
      "tune", 0x00000004, "speed-preset", 4, "bitrate", 2000, NULL);

  gst_preset_save_preset (preset, preset_name);
  gst_object_unref (preset);
}

static void
_create_default_encoding_target (GstMultiTranscodeBin * self)
{
  GstEncodingProfile *profile;
  GstEncodingProfile *child_profile;
  GstCaps *container_caps;
  GstCaps *video_caps;
  GstCaps *audio_caps;
  GstCaps *video_restrict_caps;

  self->encoding_target = gst_encoding_target_new ("default-encoding-target",
      GST_ENCODING_CATEGORY_ONLINE_SERVICE, "default-encoding-target", NULL);

  container_caps = gst_caps_new_empty_simple ("application/x-hls");
  video_caps =
      gst_caps_new_simple ("video/x-h264", "profile", G_TYPE_STRING, "main",
      NULL);
  audio_caps =
      gst_caps_new_simple ("audio/mpeg", "mpegversion", G_TYPE_INT, 4, NULL);

  /* 720p streams */
  profile = (GstEncodingProfile *) gst_encoding_container_profile_new ("720p",
      NULL, container_caps, NULL);
  video_restrict_caps = gst_caps_new_empty_simple ("video/x-raw");
  gst_caps_set_simple (video_restrict_caps,
      "width", G_TYPE_INT, 1280, "height", G_TYPE_INT, 720, "framerate",
      GST_TYPE_FRACTION, 30, 1, NULL);

  child_profile =
      (GstEncodingProfile *) gst_encoding_video_profile_new (video_caps,
      DEFAULT_X264_720P_PRESET_NAME, video_restrict_caps, 0);
  gst_caps_unref (video_restrict_caps);

  gst_encoding_container_profile_add_profile ((GstEncodingContainerProfile *)
      profile, child_profile);

  child_profile =
      (GstEncodingProfile *) gst_encoding_audio_profile_new (audio_caps, NULL,
      NULL, 0);
  gst_encoding_container_profile_add_profile ((GstEncodingContainerProfile *)
      profile, child_profile);

  gst_encoding_target_add_profile (self->encoding_target, profile);

  /* 480p streams */
  profile = (GstEncodingProfile *) gst_encoding_container_profile_new ("480p",
      NULL, container_caps, NULL);
  video_restrict_caps = gst_caps_new_empty_simple ("video/x-raw");
  gst_caps_set_simple (video_restrict_caps,
      "width", G_TYPE_INT, 640, "height", G_TYPE_INT, 480, "framerate",
      GST_TYPE_FRACTION, 30, 1, NULL);

  child_profile =
      (GstEncodingProfile *) gst_encoding_video_profile_new (video_caps,
      DEFAULT_X264_480P_PRESET_NAME, video_restrict_caps, 0);
  gst_caps_unref (video_restrict_caps);

  gst_encoding_container_profile_add_profile ((GstEncodingContainerProfile *)
      profile, child_profile);

  child_profile =
      (GstEncodingProfile *) gst_encoding_audio_profile_new (audio_caps, NULL,
      NULL, 0);
  gst_encoding_container_profile_add_profile ((GstEncodingContainerProfile *)
      profile, child_profile);

  gst_encoding_target_add_profile (self->encoding_target, profile);

  /* 240p streams */
  profile = (GstEncodingProfile *) gst_encoding_container_profile_new ("240p",
      NULL, container_caps, NULL);
  video_restrict_caps = gst_caps_new_empty_simple ("video/x-raw");
  gst_caps_set_simple (video_restrict_caps,
      "width", G_TYPE_INT, 360, "height", G_TYPE_INT, 240, "framerate",
      GST_TYPE_FRACTION, 30, 1, NULL);

  child_profile =
      (GstEncodingProfile *) gst_encoding_video_profile_new (video_caps,
      DEFAULT_X264_240P_PRESET_NAME, video_restrict_caps, 0);
  gst_caps_unref (video_restrict_caps);

  gst_encoding_container_profile_add_profile ((GstEncodingContainerProfile *)
      profile, child_profile);

  child_profile =
      (GstEncodingProfile *) gst_encoding_audio_profile_new (audio_caps, NULL,
      NULL, 0);
  gst_encoding_container_profile_add_profile ((GstEncodingContainerProfile *)
      profile, child_profile);

  gst_encoding_target_add_profile (self->encoding_target, profile);


  /* audio only streams */
  profile =
      (GstEncodingProfile *) gst_encoding_container_profile_new ("audio-only",
      NULL, container_caps, NULL);

  child_profile =
      (GstEncodingProfile *) gst_encoding_audio_profile_new (audio_caps, NULL,
      NULL, 0);
  gst_encoding_container_profile_add_profile ((GstEncodingContainerProfile *)
      profile, child_profile);

  gst_encoding_target_add_profile (self->encoding_target, profile);

  gst_caps_unref (video_caps);
  gst_caps_unref (audio_caps);
}

static void
gst_multi_transcode_bin_init (GstMultiTranscodeBin * self)
{
  self->main_input = create_new_input (self, TRUE);

  self->root_dir = g_strdup (DEFAULT_ROOT_DIR);
  self->target_duration = DEFAULT_TARGET_DURATION;

  self->num_child = 0;

  _create_default_x264_preset (DEFAULT_X264_720P_PRESET_NAME, 2000);
  _create_default_x264_preset (DEFAULT_X264_480P_PRESET_NAME, 1200);
  _create_default_x264_preset (DEFAULT_X264_240P_PRESET_NAME, 800);

  _create_default_encoding_target (self);
}

static void
gst_multi_transcode_bin_dispose (GObject * object)
{
  GstMultiTranscodeBin *self = (GstMultiTranscodeBin *) object;
  GList *walk, *next;

  free_input (self, self->main_input);

  for (walk = self->other_inputs; walk; walk = next) {
    GstTransInput *input = walk->data;

    next = g_list_next (walk);

    free_input (self, input);
    self->other_inputs = g_list_delete_link (self->other_inputs, walk);
  }

  if (self->encoding_target)
    gst_object_unref (self->encoding_target);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_multi_transcode_bin_finalize (GObject * object)
{
  GstMultiTranscodeBin *self = (GstMultiTranscodeBin *) object;

  g_free (self->root_dir);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_multi_transcode_bin_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstMultiTranscodeBin *self = GST_MULTI_TRANSCODE_BIN (object);

  switch (prop_id) {
    case PROP_ROOT_DIR:
      GST_OBJECT_LOCK (self);
      g_value_set_string (value, self->root_dir);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_TARGET_DURATION:
      GST_OBJECT_LOCK (self);
      g_value_set_uint (value, self->target_duration);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_ENCODING_TARGET:
      GST_OBJECT_LOCK (self);
      g_value_set_object (value, self->encoding_target);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_multi_transcode_bin_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstMultiTranscodeBin *self = GST_MULTI_TRANSCODE_BIN (object);

  switch (prop_id) {
    case PROP_ROOT_DIR:
      GST_OBJECT_LOCK (self);
      /* FIXME : propagate changed path to children */
      g_free (self->root_dir);
      self->root_dir = g_value_dup_string (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_TARGET_DURATION:
      GST_OBJECT_LOCK (self);
      self->target_duration = g_value_get_uint (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_ENCODING_TARGET:
      GST_OBJECT_LOCK (self);
      if (self->encoding_target)
        gst_object_unref (self->encoding_target);
      self->encoding_target = g_value_dup_object (value);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* Call with INPUT_LOCK taken */
static gboolean
ensure_input_decodebin (GstMultiTranscodeBin * self, GstTransInput * input)
{
  gboolean set_state = FALSE;

  if (input->decodebin == NULL) {
    input->decodebin = gst_element_factory_make ("decodebin", NULL);
    if (input->decodebin == NULL)
      goto no_decodebin;
    input->decodebin = gst_object_ref (input->decodebin);
    input->decodebin_sink =
        gst_element_get_static_pad (input->decodebin, "sink");
    input->pad_added_sigid =
        g_signal_connect (input->decodebin, "pad-added",
        G_CALLBACK (pad_added_cb), self);
    input->no_more_pads_sigid =
        g_signal_connect (input->decodebin, "no-more-pads",
        G_CALLBACK (no_more_pads_cb), self);
    /* TODO : if we wannt decode ourselves (not in decodebin)

     * connect autoplug-continue signla in here */
  }

  if (GST_OBJECT_PARENT (GST_OBJECT (input->decodebin)) != GST_OBJECT (self)) {
    gst_bin_add (GST_BIN (self), input->decodebin);
    set_state = TRUE;
  }

  gst_ghost_pad_set_target (GST_GHOST_PAD (input->ghost_sink),
      input->decodebin_sink);
  if (set_state)
    gst_element_sync_state_with_parent (input->decodebin);

  return TRUE;

  /* ERRORS */
no_decodebin:
  {
    gst_element_post_message ((GstElement *) self,
        gst_missing_element_message_new ((GstElement *) self, "decodebin"));
    return FALSE;
  }
}

static void
gst_multi_transcode_bin_input_pad_unlink (GstPad * pad, GstObject * parent)
{
  GstMultiTranscodeBin *self = (GstMultiTranscodeBin *) parent;
  GstTransInput *input;

  GST_LOG_OBJECT (parent, "Got unlink on input pad %" GST_PTR_FORMAT
      ". Removing parsebin.", pad);

  if ((input =
          g_object_get_data (G_OBJECT (pad),
              "multitranscodebin.input")) == NULL)
    goto fail;

  INPUT_LOCK (self);
  if (input->decodebin == NULL) {
    INPUT_UNLOCK (self);
    return;
  }

  if (GST_OBJECT_PARENT (GST_OBJECT (input->decodebin)) == GST_OBJECT (self)) {
    gst_bin_remove (GST_BIN (self), input->decodebin);
    gst_element_set_state (input->decodebin, GST_STATE_NULL);
    g_signal_handler_disconnect (input->decodebin, input->pad_added_sigid);
    g_signal_handler_disconnect (input->decodebin, input->no_more_pads_sigid);
    gst_object_unref (input->decodebin);
    gst_object_unref (input->decodebin_sink);

    input->decodebin = NULL;
    input->decodebin_sink = NULL;

    if (!input->is_main) {
      self->other_inputs = g_list_remove (self->other_inputs, input);
      free_input_async (self, input);
    }
  }
  INPUT_UNLOCK (self);
  return;

fail:
  GST_ERROR_OBJECT (parent, "Failed to retrieve input state from ghost pad");
  return;
}

static GstPadLinkReturn
gst_multi_transcode_bin_input_pad_link (GstPad * pad, GstObject * parent,
    GstPad * peer)
{
  GstMultiTranscodeBin *self = (GstMultiTranscodeBin *) parent;
  GstPadLinkReturn res = GST_PAD_LINK_OK;
  GstTransInput *input;

  GST_LOG_OBJECT (parent, "Got link on input pad %" GST_PTR_FORMAT
      ". Creating decodebin if needed", pad);

  if ((input =
          g_object_get_data (G_OBJECT (pad),
              "multitranscodebin.input")) == NULL)
    goto fail;

  INPUT_LOCK (self);
  if (!ensure_input_decodebin (self, input))
    res = GST_PAD_LINK_REFUSED;
  INPUT_UNLOCK (self);

  return res;
fail:
  GST_ERROR_OBJECT (parent, "Failed to retrieve input state from ghost pad");
  return GST_PAD_LINK_REFUSED;
}

static void
free_input (GstMultiTranscodeBin * self, GstTransInput * input)
{
  GST_DEBUG ("Freeing input %p", input);
  gst_ghost_pad_set_target (GST_GHOST_PAD (input->ghost_sink), NULL);
  gst_element_remove_pad (GST_ELEMENT (self), input->ghost_sink);
  if (input->decodebin) {
    g_signal_handler_disconnect (input->decodebin, input->pad_added_sigid);
    g_signal_handler_disconnect (input->decodebin, input->no_more_pads_sigid);
    gst_element_set_state (input->decodebin, GST_STATE_NULL);
    gst_object_unref (input->decodebin);
    gst_object_unref (input->decodebin_sink);
  }

  g_free (input);
}

static void
free_input_async (GstMultiTranscodeBin * self, GstTransInput * input)
{
  GST_LOG_OBJECT (self, "pushing input %p on thread pool to free", input);
  gst_element_call_async (GST_ELEMENT_CAST (self),
      (GstElementCallAsyncFunc) free_input, input, NULL);
}

static GstTransInput *
create_new_input (GstMultiTranscodeBin * self, gboolean main)
{
  GstTransInput *input;

  input = g_new0 (GstTransInput, 1);

  input->tbin = self;
  input->is_main = main;

  if (main)
    input->ghost_sink = gst_ghost_pad_new_no_target ("sink", GST_PAD_SINK);
  else {
    gchar *pad_name = g_strdup_printf ("sink_%u", self->input_counter++);
    input->ghost_sink = gst_ghost_pad_new_no_target (pad_name, GST_PAD_SINK);
    g_free (pad_name);
  }
  g_object_set_data (G_OBJECT (input->ghost_sink),
      "multitranscodebin.input", input);
  gst_pad_set_link_function (input->ghost_sink,
      gst_multi_transcode_bin_input_pad_link);
  gst_pad_set_unlink_function (input->ghost_sink,
      gst_multi_transcode_bin_input_pad_unlink);

  gst_pad_set_active (input->ghost_sink, TRUE);
  gst_element_add_pad ((GstElement *) self, input->ghost_sink);

  return input;
}

static GstPad *
gst_multi_transcode_bin_request_new_pad (GstElement * element,
    GstPadTemplate * temp, const gchar * name, const GstCaps * caps)
{
  GstMultiTranscodeBin *self = (GstMultiTranscodeBin *) element;
  GstTransInput *input;
  GstPad *res = NULL;

  /* We are ignoring names for the time being, not sure it makes any sense
   * within the context of decodebin3 ... */
  input = create_new_input (self, FALSE);
  if (input) {
    INPUT_LOCK (self);
    self->other_inputs = g_list_append (self->other_inputs, input);
    res = input->ghost_sink;
    INPUT_UNLOCK (self);
  }

  return res;
}

static GstStreamType
guess_stream_type_from_caps (GstCaps * caps)
{
  GstStructure *s;
  const gchar *name;

  if (gst_caps_get_size (caps) < 1)
    return GST_STREAM_TYPE_UNKNOWN;

  s = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (s);

  if (g_str_has_prefix (name, "video/") || g_str_has_prefix (name, "image/"))
    return GST_STREAM_TYPE_VIDEO;
  if (g_str_has_prefix (name, "audio/"))
    return GST_STREAM_TYPE_AUDIO;
  if (g_str_has_prefix (name, "text/") ||
      g_str_has_prefix (name, "subpicture/"))
    return GST_STREAM_TYPE_TEXT;

  return GST_STREAM_TYPE_UNKNOWN;
}

static void
post_missing_plugin_error (GstElement * dec, const gchar * element_name)
{
  GstMessage *msg;

  msg = gst_missing_element_message_new (dec, element_name);
  gst_element_post_message (dec, msg);

  GST_ELEMENT_ERROR (dec, CORE, MISSING_PLUGIN,
      ("Missing element '%s' - check your GStreamer installation.",
          element_name), (NULL));
}

static void
pad_added_cb (GstElement * decodebin, GstPad * pad, GstMultiTranscodeBin * self)
{
  GstCaps *caps;
  GstPad *sinkpad = NULL;
  GstPadLinkReturn lret;
  GstElement *tee;
  GstTransChain *transchain;
  GstStreamType stream_type;
  GList *iter;

  caps = gst_pad_query_caps (pad, NULL);

  GST_DEBUG_OBJECT (decodebin, "Pad added, caps: %" GST_PTR_FORMAT, caps);

  stream_type = guess_stream_type_from_caps (caps);

  if (caps)
    gst_caps_unref (caps);

  tee = gst_element_factory_make ("tee", NULL);

  if (!tee) {
    post_missing_plugin_error (GST_ELEMENT_CAST (self), "tee");
    return;
  }

  gst_bin_add (GST_BIN (self), tee);
  gst_element_sync_state_with_parent (tee);
  sinkpad = gst_element_get_static_pad (tee, "sink");

  lret = gst_pad_link (pad, sinkpad);
  if (G_UNLIKELY (lret != GST_PAD_LINK_OK)) {
    GstCaps *othercaps = gst_pad_query_caps (sinkpad, NULL);
    caps = gst_pad_get_current_caps (pad);

    GST_ELEMENT_ERROR_WITH_DETAILS (self, CORE, PAD,
        (NULL),
        ("Couldn't link pads:\n    %" GST_PTR_FORMAT ": %" GST_PTR_FORMAT
            "\nand:\n" "    %" GST_PTR_FORMAT ": %" GST_PTR_FORMAT "\n\n",
            pad, caps, sinkpad, othercaps),
        ("linking-error", GST_TYPE_PAD_LINK_RETURN, lret,
            "source-pad", GST_TYPE_PAD, pad,
            "source-caps", GST_TYPE_CAPS, caps,
            "sink-pad", GST_TYPE_PAD, sinkpad,
            "sink-caps", GST_TYPE_CAPS, othercaps, NULL));

    gst_caps_unref (caps);
    if (othercaps)
      gst_caps_unref (othercaps);

    gst_object_unref (sinkpad);
    gst_object_unref (tee);
    return;
  }

  transchain = g_slice_new0 (GstTransChain);
  transchain->sinkpad = sinkpad;
  transchain->tee = tee;
  transchain->parent = self;

  self->transchain = g_list_append (self->transchain, transchain);

  for (iter = self->childs; iter; iter = g_list_next (iter)) {
    GstTransChild *child = (GstTransChild *) iter->data;
    GstPad *srcpad = NULL;

    if (stream_type == GST_STREAM_TYPE_VIDEO && child->video_sink) {
      srcpad = gst_element_get_request_pad (tee, "src_%u");
      lret = gst_pad_link (srcpad, child->video_sink);

      if (G_UNLIKELY (lret != GST_PAD_LINK_OK)) {
        GST_ELEMENT_ERROR (self, CORE, PAD, (NULL),
            ("Couldn't link pads:\n    %" GST_PTR_FORMAT " and %"
                GST_PTR_FORMAT, srcpad, child->video_sink));
      }
    } else if (stream_type == GST_STREAM_TYPE_AUDIO && child->audio_sink) {
      srcpad = gst_element_get_request_pad (tee, "src_%u");
      lret = gst_pad_link (srcpad, child->audio_sink);

      if (G_UNLIKELY (lret != GST_PAD_LINK_OK)) {
        GST_ELEMENT_ERROR (self, CORE, PAD, (NULL),
            ("Couldn't link pads:\n    %" GST_PTR_FORMAT " and %"
                GST_PTR_FORMAT, srcpad, child->audio_sink));
      }
    }

    if (srcpad)
      gst_object_unref (srcpad);
  }
}

/* All pads of decodebin are exposed and we can finish child configuration */
static void
no_more_pads_cb (GstElement * decodebin, GstMultiTranscodeBin * self)
{
  /* Emit decodebin-setup signal */
  GST_DEBUG_OBJECT (decodebin, "got no-more-pads from decodebin");
  g_signal_emit (self, gst_multi_transcode_bin_signals[SIGNAL_DECODEBIN_SETUP],
      0, decodebin);

  /* TODO: connect decodebin with child bins */
}

static gboolean
configure_audio_encoding (GstMultiTranscodeBin * self, GstTransChild * child,
    GstEncodingAudioProfile * profile)
{
  GstElement *encodebin;
  gboolean ret;
  GstPad *pad, *opad;
  GstPad *ghost_pad;

  encodebin = gst_element_factory_make ("encodebin", NULL);
  gst_bin_add (GST_BIN (child->bin), encodebin);
  g_object_set (encodebin, "profile", profile, NULL);

  pad = gst_element_get_static_pad (encodebin, "audio_0");

  if (pad == NULL) {
    GstCaps *caps =
        gst_encoding_profile_get_format ((GstEncodingProfile *) profile);
    GST_ELEMENT_WARNING_WITH_DETAILS (self, STREAM, FORMAT,
        (NULL), ("Stream with caps: %" GST_PTR_FORMAT " can not be"
            " encoded in the defined encoding formats",
            caps),
        ("can-t-encode-stream", G_TYPE_BOOLEAN, TRUE,
            "stream-caps", GST_TYPE_CAPS, caps, NULL));

    if (caps)
      gst_caps_unref (caps);

    return FALSE;
  }

  ghost_pad = gst_ghost_pad_new ("audiosink", pad);
  gst_object_unref (pad);

  child->audio_sink = ghost_pad;

  gst_pad_set_active (ghost_pad, TRUE);
  ret = gst_element_add_pad (GST_ELEMENT_CAST (child->bin), ghost_pad);
  if (!ret) {
    GST_ERROR_OBJECT (self,
        "Failed to add %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT,
        ghost_pad, child->bin);

    return FALSE;
  }

  pad = gst_element_get_request_pad (child->sink, "audio");
  opad = gst_element_get_static_pad (encodebin, "src");
  gst_pad_link (opad, pad);
  gst_object_unref (pad);
  gst_object_unref (opad);

  gst_element_sync_state_with_parent (encodebin);

  return TRUE;
}

static gboolean
configure_video_encoding (GstMultiTranscodeBin * self, GstTransChild * child,
    GstEncodingVideoProfile * profile)
{
  GstElement *encodebin;
  gboolean ret;
  GstPad *pad, *opad;
  GstPad *ghost_pad;

  encodebin = gst_element_factory_make ("encodebin", NULL);
  gst_bin_add (GST_BIN (child->bin), encodebin);
  g_object_set (encodebin, "profile", profile, NULL);

  pad = gst_element_get_static_pad (encodebin, "video_0");

  if (pad == NULL) {
    GstCaps *caps =
        gst_encoding_profile_get_format ((GstEncodingProfile *) profile);
    GST_ELEMENT_WARNING_WITH_DETAILS (self, STREAM, FORMAT,
        (NULL), ("Stream with caps: %" GST_PTR_FORMAT " can not be"
            " encoded in the defined encoding formats",
            caps),
        ("can-t-encode-stream", G_TYPE_BOOLEAN, TRUE,
            "stream-caps", GST_TYPE_CAPS, caps, NULL));
    if (caps)
      gst_caps_unref (caps);

    return FALSE;
  }

  ghost_pad = gst_ghost_pad_new ("videosink", pad);
  gst_object_unref (pad);

  child->video_sink = ghost_pad;

  gst_pad_set_active (ghost_pad, TRUE);
  ret = gst_element_add_pad (GST_ELEMENT_CAST (child->bin), ghost_pad);
  if (!ret) {
    GST_ERROR_OBJECT (self,
        "Failed to add %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT,
        ghost_pad, child->bin);

    return FALSE;
  }

  pad = gst_element_get_request_pad (child->sink, "video");
  opad = gst_element_get_static_pad (encodebin, "src");
  gst_pad_link (opad, pad);
  gst_object_unref (pad);
  gst_object_unref (opad);

  gst_element_sync_state_with_parent (encodebin);

  return TRUE;
}

static gboolean
make_single_childbin (GstMultiTranscodeBin * self, GstEncodingProfile * profile)
{
  GstTransChild *child = g_slice_new0 (GstTransChild);
  gboolean ret;
  gchar *child_name = g_strdup_printf ("transchild_%u", self->num_child);
  gchar *location;
  gchar *playlist_location;
  const gchar *profile_name;
  GstCaps *profile_caps;
  GstStructure *s;
  const gchar *name;

  child->bin = gst_bin_new (child_name);

  if (!GST_IS_ENCODING_CONTAINER_PROFILE (profile)) {
    GST_ERROR_OBJECT (self, "Unexpected encoding profile");
    g_free (child_name);
    return FALSE;
  }
  g_free (child_name);

  profile_caps = gst_encoding_profile_get_format (profile);
  if (!profile_caps) {
    GST_ERROR_OBJECT (self, "Encoding profile has no format");
    return FALSE;
  }

  profile_name = gst_encoding_profile_get_name (profile);
  if (!g_file_test (profile_name, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR))
    g_mkdir (profile_name, S_IRWXU | S_IRWXG | S_IRWXO);

  s = gst_caps_get_structure (profile_caps, 0);
  name = gst_structure_get_name (s);

  if (g_str_has_prefix (name, "application/x-hls")) {
    GList *child_profiles =
        (GList *) gst_encoding_container_profile_get_profiles (
        (GstEncodingContainerProfile *) profile);
    GList *iter;

    GST_DEBUG_OBJECT (self, "Configure hls childbin");

    child->sink = gst_element_factory_make ("hlssink2", NULL);
    gst_bin_add (GST_BIN (child->bin), child->sink);
    location =
        g_strdup_printf ("%s/%s%s.ts", profile_name, profile_name, "%05d");
    playlist_location = g_strdup_printf ("%s/playlist.m3u8", profile_name);

    g_object_set (G_OBJECT (child->sink),
        "location", location,
        "playlist-location", playlist_location,
        "target-duration", self->target_duration, NULL);
    g_free (location);
    g_free (playlist_location);

    for (iter = child_profiles; iter; iter = g_list_next (iter)) {
      if (GST_IS_ENCODING_VIDEO_PROFILE (iter->data)) {

        if (!configure_video_encoding (self, child,
                (GstEncodingVideoProfile *) iter->data))
          return FALSE;
      } else if (GST_IS_ENCODING_AUDIO_PROFILE (iter->data)) {
        if (!configure_audio_encoding (self, child,
                (GstEncodingAudioProfile *) iter->data))
          return FALSE;
      } else {
        GST_ERROR_OBJECT (self, "Unknown encoding profile");
        return FALSE;
      }

    }

  } else {
    child->sink = gst_element_factory_make ("filesink", NULL);
    /* TODO: make encodebin */
  }

  ret = gst_bin_add (GST_BIN (self), GST_ELEMENT_CAST (child->bin));
  if (!ret) {
    GST_ERROR_OBJECT (self,
        "Failed to add %" GST_PTR_FORMAT " to %" GST_PTR_FORMAT,
        child->bin, self);

    return FALSE;
  }

  ret = gst_element_sync_state_with_parent (GST_ELEMENT_CAST (child->bin));
  if (!ret) {
    GST_ERROR_OBJECT (self, "Failed to sync with parent");

    return FALSE;
  }

  self->childs = g_list_append (self->childs, child);
  self->num_child++;

  gst_caps_unref (profile_caps);

  return TRUE;
}

static gboolean
make_childbins (GstMultiTranscodeBin * self)
{
  GList *profiles;
  GList *iter;

  GST_INFO_OBJECT (self, "making new childbins");
  if (!self->encoding_target) {
    GST_ERROR_OBJECT (self, "No encoding-target specified");
    return FALSE;
  }

  profiles = (GList *) gst_encoding_target_get_profiles (self->encoding_target);
  GST_DEBUG_OBJECT (self, "Have %d encoding profiles",
      g_list_length (profiles));
  for (iter = profiles; iter; iter = g_list_next (iter))
    make_single_childbin (self, (GstEncodingProfile *) iter->data);

  return TRUE;
}

static GstStateChangeReturn
gst_multi_transcode_bin_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstMultiTranscodeBin *self = GST_MULTI_TRANSCODE_BIN (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (!make_childbins (self))
        goto setup_failed;
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  return ret;

setup_failed:
  return GST_STATE_CHANGE_FAILURE;
}

gboolean
gst_multi_transcode_bin_plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_multi_transcodebin_debug, "multitranscodebin", 0,
      "Multi Transcodebin element");

  return gst_element_register (plugin, "multitranscodebin", GST_RANK_NONE,
      GST_TYPE_MULTI_TRANSCODE_BIN);
}
