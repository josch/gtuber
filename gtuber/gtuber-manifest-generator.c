/*
 * Copyright (C) 2021 Rafał Dzięgiel <rafostar.github@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:gtuber-manifest-generator
 * @title: GtuberManifestGenerator
 * @short_description: generates adaptive streams manifest
 *   from media info
 */

#include "gtuber-enums.h"
#include "gtuber-manifest-generator.h"
#include "gtuber-stream.h"

struct _GtuberManifestGenerator
{
  GObject parent;

  gboolean pretty;
  guint indent;

  GtuberMediaInfo *media_info;

  GtuberAdaptiveStreamFilter filter_func;
  gpointer filter_data;
  GDestroyNotify filter_destroy;
};

struct _GtuberManifestGeneratorClass
{
  GObjectClass parent_class;
};

typedef enum
{
  DASH_CODEC_UNKNOWN,
  DASH_CODEC_AVC,
  DASH_CODEC_HEVC,
  DASH_CODEC_VP9,
  DASH_CODEC_AV1,
  DASH_CODEC_MP4A,
  DASH_CODEC_OPUS,
} DashCodec;

#define parent_class gtuber_manifest_generator_parent_class
G_DEFINE_TYPE (GtuberManifestGenerator, gtuber_manifest_generator, G_TYPE_OBJECT)
G_DEFINE_QUARK (gtubermanifestgenerator-error-quark, gtuber_manifest_generator_error)

static void gtuber_manifest_generator_dispose (GObject *object);
static void gtuber_manifest_generator_finalize (GObject *object);

static void
gtuber_manifest_generator_init (GtuberManifestGenerator *self)
{
  self->pretty = FALSE;
  self->indent = 2;

  self->media_info = NULL;

  self->filter_func = NULL;
  self->filter_destroy = NULL;
}

static void
gtuber_manifest_generator_class_init (GtuberManifestGeneratorClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->dispose = gtuber_manifest_generator_dispose;
  gobject_class->finalize = gtuber_manifest_generator_finalize;
}

static void
gtuber_manifest_generator_dispose (GObject *object)
{
  GtuberManifestGenerator *self = GTUBER_MANIFEST_GENERATOR (object);

  if (self->filter_destroy)
    self->filter_destroy (self->filter_data);

  self->filter_func = NULL;
  self->filter_destroy = NULL;

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gtuber_manifest_generator_finalize (GObject *object)
{
  GtuberManifestGenerator *self = GTUBER_MANIFEST_GENERATOR (object);

  g_debug ("ManifestGenerator finalize");

  if (self->media_info)
    g_object_unref (self->media_info);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static DashCodec
get_dash_video_codec (const gchar *codec)
{
  DashCodec dash_codec = DASH_CODEC_UNKNOWN;

  if (codec) {
    if (g_str_has_prefix (codec, "avc"))
      dash_codec = DASH_CODEC_AVC;
    else if (g_str_has_prefix (codec, "vp9"))
      dash_codec = DASH_CODEC_VP9;
    else if (g_str_has_prefix (codec, "hev"))
      dash_codec = DASH_CODEC_HEVC;
    else if (g_str_has_prefix (codec, "av01"))
      dash_codec = DASH_CODEC_AV1;
  }

  return dash_codec;
}

static DashCodec
get_dash_audio_codec (const gchar *codec)
{
  DashCodec dash_codec = DASH_CODEC_UNKNOWN;

  if (codec) {
    if (g_str_has_prefix (codec, "mp4a"))
      dash_codec = DASH_CODEC_MP4A;
    else if (g_str_has_prefix (codec, "opus"))
      dash_codec = DASH_CODEC_OPUS;
  }

  return dash_codec;
}

static guint
_get_gcd (guint width, guint height)
{
  return (height > 0)
      ? _get_gcd (height, width % height)
      : width;
}

static gchar *
obtain_par_from_res (guint width, guint height)
{
  guint gcd;

  if (!width || !height)
    return g_strdup ("1:1");

  gcd = _get_gcd (width, height);

  width /= gcd;
  height /= gcd;

  return g_strdup_printf ("%i:%i", width, height);
}

typedef struct
{
  GtuberStreamMimeType mime_type;
  DashCodec dash_codec;
  guint max_width;
  guint max_height;
  guint max_fps;
  GPtrArray *adaptive_streams;
} DashAdaptationData;

static DashAdaptationData *
dash_adaptation_data_new (GtuberStreamMimeType mime_type, DashCodec dash_codec)
{
  DashAdaptationData *data;

  data = g_new (DashAdaptationData, 1);
  data->mime_type = mime_type;
  data->dash_codec = dash_codec;
  data->max_width = 0;
  data->max_height = 0;
  data->max_fps = 0;
  data->adaptive_streams =
      g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

  return data;
}

static void
dash_adaptation_data_free (DashAdaptationData *data)
{
  g_ptr_array_unref (data->adaptive_streams);
  g_free (data);
}

static DashAdaptationData *
get_adaptation_data_for_stream (GtuberStream *stream, GPtrArray *adaptations)
{
  GtuberStreamMimeType mime_type;
  DashCodec dash_codec = DASH_CODEC_UNKNOWN;
  const gchar *codec = NULL;
  guint i;

  mime_type = gtuber_stream_get_mime_type (stream);

  switch (mime_type) {
    case GTUBER_STREAM_MIME_TYPE_VIDEO_MP4:
    case GTUBER_STREAM_MIME_TYPE_VIDEO_WEBM:
      codec = gtuber_stream_get_video_codec (stream);
      dash_codec = get_dash_video_codec (codec);
      break;
    case GTUBER_STREAM_MIME_TYPE_AUDIO_MP4:
    case GTUBER_STREAM_MIME_TYPE_AUDIO_WEBM:
      codec = gtuber_stream_get_audio_codec (stream);
      dash_codec = get_dash_audio_codec (codec);
      break;
    default:
      break;
  }

  if (codec && dash_codec != DASH_CODEC_UNKNOWN) {
    DashAdaptationData *data = NULL;

    for (i = 0; i < adaptations->len; i++) {
      data = g_ptr_array_index (adaptations, i);
      if (data->mime_type == mime_type && data->dash_codec == dash_codec)
        return data;
    }

    /* No such adaptation yet in array, so create
     * a new one, add it to array and return it */
    data = dash_adaptation_data_new (mime_type, dash_codec);
    g_ptr_array_add (adaptations, data);

    return data;
  }

  g_debug ("Cannot create adaptation data for unknown codec: %s", codec);

  return NULL;
}

static void
add_line_no_newline (GtuberManifestGenerator *self, GString *string,
    guint depth, const gchar *line)
{
  guint indent = depth * self->indent;

  if (self->pretty) {
    while (indent--)
      g_string_append (string, " ");
  }

  g_string_append (string, line);
}

static void
finish_line (GtuberManifestGenerator *self, GString *string, const gchar *suffix)
{
  g_string_append_printf (string, "%s%s",
      suffix ? suffix : "", self->pretty ? "\n" : "");
}

static void
add_line (GtuberManifestGenerator *self, GString *string,
    guint depth, const gchar *line)
{
  add_line_no_newline (self, string, depth, line);
  finish_line (self, string, NULL);
}

static void
add_option_string (GString *string, const gchar *key, const gchar *value)
{
  g_string_append_printf (string, " %s=\"%s\"", key, value);
}

static void
add_option_int (GString *string, const gchar *key, guint64 value)
{
  g_string_append_printf (string, " %s=\"%lu\"", key, value);
}

static void
add_option_range (GString *string, const gchar *key, guint64 start, guint64 end)
{
  g_string_append_printf (string, " %s=\"%lu-%lu\"", key, start, end);
}

static void
add_option_boolean (GString *string, const gchar *key, gboolean value)
{
  const gchar *boolean_str;

  boolean_str = (value) ? "true" : "false";
  g_string_append_printf (string, " %s=\"%s\"", key, boolean_str);
}

static void
parse_content_and_mime_type (GtuberStreamMimeType mime_type,
    gchar **content_str, gchar **mime_str)
{
  switch (mime_type) {
    case GTUBER_STREAM_MIME_TYPE_VIDEO_MP4:
    case GTUBER_STREAM_MIME_TYPE_VIDEO_WEBM:
      *content_str = g_strdup ("video");
      break;
    case GTUBER_STREAM_MIME_TYPE_AUDIO_MP4:
    case GTUBER_STREAM_MIME_TYPE_AUDIO_WEBM:
      *content_str = g_strdup ("audio");
      break;
    default:
      *content_str = NULL;
      break;
  }

  switch (mime_type) {
    case GTUBER_STREAM_MIME_TYPE_VIDEO_MP4:
    case GTUBER_STREAM_MIME_TYPE_AUDIO_MP4:
      *mime_str = g_strdup_printf ("%s/%s", *content_str, "mp4");
      break;
    case GTUBER_STREAM_MIME_TYPE_VIDEO_WEBM:
    case GTUBER_STREAM_MIME_TYPE_AUDIO_WEBM:
      *mime_str = g_strdup_printf ("%s/%s", *content_str, "webm");
      break;
    default:
      *mime_str = NULL;
      break;
  }
}

typedef struct
{
  GtuberManifestGenerator *gen;
  GString *string;
} DumpStringData;

static DumpStringData *
dump_string_data_new (GtuberManifestGenerator *gen, GString *string)
{
  DumpStringData *data;

  data = g_new (DumpStringData, 1);
  data->gen = g_object_ref (gen);
  data->string = string;

  return data;
}

static void
dump_string_data_free (DumpStringData *data)
{
  g_object_unref (data->gen);
  g_free (data);
}

static void
_add_representation_cb (GtuberAdaptiveStream *astream, DumpStringData *data)
{
  GtuberStream *stream;
  const gchar *vcodec, *acodec;
  guint width, height, fps;
  guint64 start, end;

  stream = GTUBER_STREAM (astream);

  width = gtuber_stream_get_width (stream);
  height = gtuber_stream_get_height (stream);
  fps = gtuber_stream_get_fps (stream);

  /* <Representation> */
  add_line_no_newline (data->gen, data->string, 3, "<Representation");
  add_option_int (data->string, "id", gtuber_stream_get_itag (stream));

  if (gtuber_stream_get_codecs (stream, &vcodec, &acodec)) {
    if (vcodec && acodec) {
      gchar *codecs_str;

      codecs_str = g_strdup_printf ("%s, %s", vcodec, acodec);
      add_option_string (data->string, "codecs", codecs_str);

      g_free (codecs_str);
    } else {
      if (vcodec)
        add_option_string (data->string, "codecs", vcodec);
      else
        add_option_string (data->string, "codecs", acodec);
    }
  }
  add_option_int (data->string, "bandwidth", gtuber_stream_get_bitrate (stream));

  if (width)
    add_option_int (data->string, "width", width);
  if (height)
    add_option_int (data->string, "height", height);
  if (width && height)
    add_option_string (data->string, "sar", "1:1");
  if (fps)
    add_option_int (data->string, "frameRate", fps);

  finish_line (data->gen, data->string, ">");

  /* TODO: Audio channels config (requires detection in stream):
   * <AudioChannelConfiguration schemeIdUri="urn:mpeg:dash:23003:3:audio_channel_configuration:2011" value="2"/>
   */

  /* <BaseURL> */
  add_line_no_newline (data->gen, data->string, 4, "<BaseURL>");
  add_line_no_newline (data->gen, data->string, 0, gtuber_stream_get_uri (stream));
  finish_line (data->gen, data->string, "</BaseURL>");

  /* <SegmentBase> */
  add_line_no_newline (data->gen, data->string, 4, "<SegmentBase");
  if (gtuber_adaptive_stream_get_index_range (astream, &start, &end))
    add_option_range (data->string, "indexRange", start, end);
  add_option_boolean (data->string, "indexRangeExact", TRUE);
  finish_line (data->gen, data->string, ">");

  /* <Initialization> */
  add_line_no_newline (data->gen, data->string, 5, "<Initialization");
  if (gtuber_adaptive_stream_get_init_range (astream, &start, &end))
    add_option_range (data->string, "range", start, end);
  finish_line (data->gen, data->string, "/>");

  add_line (data->gen, data->string, 4, "</SegmentBase>");
  add_line (data->gen, data->string, 3, "</Representation>");
}

static void
_add_adaptation_set_cb (DashAdaptationData *adaptation, DumpStringData *data)
{
  gchar *content_str, *mime_str;

  parse_content_and_mime_type (adaptation->mime_type, &content_str, &mime_str);

  if (!content_str || !mime_str) {
    g_debug ("Adaptation is missing contentType or mimeType, ignoring it");
    goto finish;
  }

  add_line_no_newline (data->gen, data->string, 2, "<AdaptationSet");
  add_option_string (data->string, "contentType", content_str);
  add_option_string (data->string, "mimeType", mime_str);
  add_option_boolean (data->string, "subsegmentAlignment", TRUE);
  add_option_int (data->string, "subsegmentStartsWithSAP", 1);
  if (!strcmp (content_str, "video")) {
    gchar *par;

    add_option_int (data->string, "maxWidth", adaptation->max_width);
    add_option_int (data->string, "maxHeight", adaptation->max_height);

    par = obtain_par_from_res (adaptation->max_width, adaptation->max_height);
    add_option_string (data->string, "par", par);
    g_free (par);

    add_option_int (data->string, "maxFrameRate", adaptation->max_fps);
  }
  finish_line (data->gen, data->string, ">");

  /* Add representations */
  g_ptr_array_foreach (adaptation->adaptive_streams, (GFunc) _add_representation_cb, data);

  add_line (data->gen, data->string, 2, "</AdaptationSet>");

finish:
  g_free (content_str);
  g_free (mime_str);
}

static gchar *
obtain_time_as_pts (guint value)
{
  return g_strdup_printf ("PT%uS", value);
}

typedef struct
{
  GtuberManifestGenerator *gen;
  GPtrArray *adaptations;
} SortStreamsData;

static SortStreamsData *
sort_streams_data_new (GtuberManifestGenerator *gen, GPtrArray *adaptations)
{
  SortStreamsData *data;

  data = g_new (SortStreamsData, 1);
  data->gen = g_object_ref (gen);
  data->adaptations = g_ptr_array_ref (adaptations);

  return data;
}

static void
sort_streams_data_free (SortStreamsData *data)
{
  g_object_unref (data->gen);
  g_ptr_array_unref (data->adaptations);
  g_free (data);
}

static void
_sort_streams_cb (GtuberAdaptiveStream *astream, SortStreamsData *sort_data)
{
  GtuberManifestGenerator *self = sort_data->gen;
  gboolean add = TRUE;

  if (self->filter_func)
    add = self->filter_func (astream, self->filter_data);

  if (add) {
    GtuberStream *stream;
    DashAdaptationData *data;

    stream = GTUBER_STREAM (astream);

    data = get_adaptation_data_for_stream (stream, sort_data->adaptations);
    if (data) {
      data->max_width = MAX (data->max_width, gtuber_stream_get_width (stream));
      data->max_height = MAX (data->max_height, gtuber_stream_get_height (stream));
      data->max_fps = MAX (data->max_fps, gtuber_stream_get_fps (stream));

      g_ptr_array_add (data->adaptive_streams, g_object_ref (astream));
    }
  }
}

static void
dump_data (GtuberManifestGenerator *self, GString *string)
{
  DumpStringData *data;
  SortStreamsData *sort_data;

  GPtrArray *adaptations;
  const GPtrArray *astreams;
  gchar *dur_pts, *buf_pts;
  guint buf_time, duration;

  g_debug ("Generating manifest data...");

  adaptations =
      g_ptr_array_new_with_free_func ((GDestroyNotify) dash_adaptation_data_free);

  astreams = gtuber_media_info_get_adaptive_streams (self->media_info);
  sort_data = sort_streams_data_new (self, adaptations);

  g_ptr_array_foreach ((GPtrArray *) astreams, (GFunc) _sort_streams_cb, sort_data);

  sort_streams_data_free (sort_data);

  if (!adaptations->len) {
    g_debug ("Adaptations array is empty");
    goto finish;
  }

  /* <xml> */
  add_line_no_newline (self, string, 0, "<?xml");
  add_option_string (string, "version", "1.0");
  add_option_string (string, "encoding", "UTF-8");
  finish_line (self, string, "?>");

  /* <MPD> */
  duration = gtuber_media_info_get_duration (self->media_info);
  dur_pts = obtain_time_as_pts (duration);

  buf_time = MIN (2, duration);
  buf_pts = obtain_time_as_pts (buf_time);

  add_line_no_newline (self, string, 0, "<MPD");
  add_option_string (string, "xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance");
  add_option_string (string, "xmlns", "urn:mpeg:dash:schema:mpd:2011");
  add_option_string (string, "xsi:schemaLocation", "urn:mpeg:dash:schema:mpd:2011 DASH-MPD.xsd");
  add_option_string (string, "type", "static");
  add_option_string (string, "mediaPresentationDuration", dur_pts);
  add_option_string (string, "minBufferTime", buf_pts);
  add_option_string (string, "profiles", "urn:mpeg:dash:profile:isoff-on-demand:2011");
  finish_line (self, string, ">");

  g_free (dur_pts);
  g_free (buf_pts);

  /* <Period> */
  add_line (self, string, 1, "<Period>");

  data = dump_string_data_new (self, string);
  g_ptr_array_foreach (adaptations, (GFunc) _add_adaptation_set_cb, data);
  dump_string_data_free (data);

  add_line (self, string, 1, "</Period>");
  add_line_no_newline (self, string, 0, "</MPD>");

  g_debug ("Manifest data generated");

finish:
  g_ptr_array_unref (adaptations);
}

static gchar *
gen_to_data_internal (GtuberManifestGenerator *self, gsize *length)
{
  GString *string;

  g_return_val_if_fail (GTUBER_IS_MANIFEST_GENERATOR (self), NULL);
  g_return_val_if_fail (self->media_info != NULL, NULL);

  string = g_string_new ("");
  dump_data (self, string);

  if (length)
    *length = string->len;

  return g_string_free (string, FALSE);
}

/**
 * gtuber_manifest_generator_new:
 *
 * Creates a new #GtuberManifestGenerator instance.
 *
 * Returns: (transfer full): a new #GtuberManifestGenerator instance.
 */
GtuberManifestGenerator *
gtuber_manifest_generator_new (void)
{
  return g_object_new (GTUBER_TYPE_MANIFEST_GENERATOR, NULL);
}

/**
 * gtuber_manifest_generator_set_media_info:
 * @gen: a #GtuberManifestGenerator
 * @info: a #GtuberMediaInfo
 *
 * Set media info used to generate the manifest data.
 * Generator will take a reference on the passed media info object.
 */
void
gtuber_manifest_generator_set_media_info (GtuberManifestGenerator *self, GtuberMediaInfo *info)
{
  g_return_if_fail (GTUBER_IS_MANIFEST_GENERATOR (self));
  g_return_if_fail (GTUBER_IS_MEDIA_INFO (info));

  if (self->media_info)
    g_object_unref (self->media_info);

  self->media_info = g_object_ref (info);
}

/**
 * gtuber_manifest_generator_set_filter_func:
 * @gen: a #GtuberManifestGenerator
 * @filter: (nullable): the filter function to use
 * @user_data: (nullable): user data passed to the filter function
 * @destroy: (nullable): destroy notifier for @user_data
 *
 * Sets the #GtuberAdaptiveStream filtering function.
 *
 * The filter function will be called for each #GtuberAdaptiveStream
 * that generator considers adding during manifest generation.
 */
void
gtuber_manifest_generator_set_filter_func (GtuberManifestGenerator *self,
    GtuberAdaptiveStreamFilter filter, gpointer user_data, GDestroyNotify destroy)
{
  g_return_if_fail (GTUBER_IS_MANIFEST_GENERATOR (self));
  g_return_if_fail (filter || (user_data == NULL && !destroy));

  if (self->filter_destroy)
    self->filter_destroy (self->filter_data);

  self->filter_func = filter;
  self->filter_data = user_data;
  self->filter_destroy = destroy;
}

/**
 * gtuber_manifest_generator_to_data:
 * @gen: a #GtuberManifestGenerator
 *
 * Generates manifest data from #GtuberMediaInfo currently set
 * in #GtuberManifestGenerator and returns it as a buffer.
 *
 * Returns: (transfer full): a newly allocated string holding manifest data.
 */
gchar *
gtuber_manifest_generator_to_data (GtuberManifestGenerator *self)
{
  return gen_to_data_internal (self, NULL);
}

/**
 * gtuber_manifest_generator_to_file:
 * @gen: a #GtuberManifestGenerator
 * @filename: (type filename): the path to the target file
 * @error: return location for a #GError, or %NULL
 *
 * Generates manifest data from #GtuberMediaInfo currently set
 * in #GtuberManifestGenerator and puts it inside `filename`,
 * overwriting the file's current contents.
 *
 * This operation is atomic, in the sense that the data is written to
 * a temporary file which is then renamed to the given `filename`.
 *
 * Returns: %TRUE if saving was successful, %FALSE otherwise.
 */
gboolean
gtuber_manifest_generator_to_file (GtuberManifestGenerator *self,
    const gchar *filename, GError **error)
{
  gchar *data;
  gsize len;
  gboolean success;

  g_return_val_if_fail (GTUBER_IS_MANIFEST_GENERATOR (self), FALSE);
  g_return_val_if_fail (filename != NULL, FALSE);

  data = gen_to_data_internal (self, &len);
  success = g_file_set_contents (filename, data, len, error);
  g_free (data);

  return success;
}