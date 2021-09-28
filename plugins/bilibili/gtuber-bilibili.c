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

#include <json-glib/json-glib.h>

#include "gtuber-bilibili.h"
#include "utils/common/gtuber-utils-common.h"
#include "utils/json/gtuber-utils-json.h"

#define parent_class gtuber_bilibili_parent_class
G_DEFINE_TYPE (GtuberBilibili, gtuber_bilibili, GTUBER_TYPE_WEBSITE)

static void gtuber_bilibili_finalize (GObject *object);

static GtuberFlow gtuber_bilibili_create_request (GtuberWebsite *website,
    GtuberMediaInfo *info, SoupMessage **msg, GError **error);
static GtuberFlow gtuber_bilibili_parse_input_stream (GtuberWebsite *website,
    GInputStream *stream, GtuberMediaInfo *info, GError **error);

static void
gtuber_bilibili_init (GtuberBilibili *self)
{
}

static void
gtuber_bilibili_class_init (GtuberBilibiliClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GtuberWebsiteClass *website_class = (GtuberWebsiteClass *) klass;

  gobject_class->finalize = gtuber_bilibili_finalize;

  website_class->handles_input_stream = TRUE;
  website_class->create_request = gtuber_bilibili_create_request;
  website_class->parse_input_stream = gtuber_bilibili_parse_input_stream;
}

static void
gtuber_bilibili_finalize (GObject *object)
{
  GtuberBilibili *self = GTUBER_BILIBILI (object);

  g_debug ("Bilibili finalize");
  g_free (self->video_id);
  g_free (self->bvid);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static const gchar *
_get_id_name (GtuberBilibili *self)
{
  switch (self->bili_type) {
    case BILIBILI_BV:
      return "bvid";
    case BILIBILI_AV:
      return "aid";
    case BILIBILI_BANGUMI_EP:
      return "ep_id";
    case BILIBILI_BANGUMI_SS:
      return "season_id";
    default:
      break;
  }

  return NULL;
}

static void
_add_dash_media_stream (GtuberBilibili *self, JsonReader *reader,
    GtuberMediaInfo *info, const gchar *type)
{
  GtuberAdaptiveStream *astream;
  GtuberStream *stream;
  GtuberStreamMimeType mime_type;
  const gchar *fps_str, *init_range, *index_range;
  gboolean is_video = strcmp (type, "video") == 0;
  guint itag = 0;

  astream = gtuber_adaptive_stream_new ();
  stream = GTUBER_STREAM (astream);

  itag = gtuber_utils_json_get_int (reader, "id", NULL)
      + gtuber_utils_json_get_int (reader, "codecid", NULL);
  gtuber_stream_set_itag (stream, itag);
  g_debug ("Created adaptive stream, itag %i", itag);

  mime_type = gtuber_utils_common_get_mime_type_from_string (
      gtuber_utils_json_get_string (reader, "mime_type", NULL));
  gtuber_stream_set_mime_type (stream, mime_type);

  if (is_video) {
    gtuber_stream_set_video_codec (stream,
        gtuber_utils_json_get_string (reader, "codecs", NULL));
  } else {
    gtuber_stream_set_audio_codec (stream,
        gtuber_utils_json_get_string (reader, "codecs", NULL));
  }

  init_range = gtuber_utils_json_get_string (reader, "segment_base", "initialization", NULL);
  if (init_range) {
    gchar **parts = g_strsplit (init_range, "-", 3);
    if (parts[0] && parts[1] && !parts[2]) {
      gtuber_adaptive_stream_set_init_range (astream,
          g_ascii_strtod (parts[0], NULL), g_ascii_strtod (parts[1], NULL));
    }
    g_strfreev (parts);
  }

  index_range = gtuber_utils_json_get_string (reader, "segment_base", "index_range", NULL);
  if (index_range) {
    gchar **parts = g_strsplit (index_range, "-", 3);
    if (parts[0] && parts[1] && !parts[2]) {
      gtuber_adaptive_stream_set_index_range (astream,
          g_ascii_strtod (parts[0], NULL), g_ascii_strtod (parts[1], NULL));
    }
    g_strfreev (parts);
  }

  /* FIXME: Add precise FPS number function */
  fps_str = gtuber_utils_json_get_string (reader, "frame_rate", NULL);
  gtuber_stream_set_fps (stream, g_ascii_strtod (fps_str, NULL));

  gtuber_stream_set_bitrate (stream, gtuber_utils_json_get_int (reader, "bandwidth", NULL));
  gtuber_stream_set_width (stream, gtuber_utils_json_get_int (reader, "width", NULL));
  gtuber_stream_set_height (stream, gtuber_utils_json_get_int (reader, "height", NULL));
  gtuber_stream_set_uri (stream, gtuber_utils_json_get_string (reader, "base_url", NULL));

  gtuber_media_info_add_adaptive_stream (info, astream);
}

static void
_add_dash_media_streams (GtuberBilibili *self, JsonReader *reader,
    GtuberMediaInfo *info, const gchar *type)
{
  guint count, i;

  /* Must be inside "dash" JSON key here */
  if (json_reader_read_member (reader, type)
      && json_reader_is_array (reader)) {
    count = json_reader_count_elements (reader);
    for (i = 0; i < count; i++) {
      if (json_reader_read_element (reader, i)) {
        _add_dash_media_stream (self, reader, info, type);
      }
      json_reader_end_element (reader);
    }
  }
  json_reader_end_member (reader);
}

static GtuberFlow
parse_info (GtuberBilibili *self, JsonParser *parser,
    GtuberMediaInfo *info, GError **error)
{
  JsonReader *reader = json_reader_new (json_parser_get_root (parser));
  GtuberFlow res = GTUBER_FLOW_ERROR;

  switch (self->bili_type) {
    case BILIBILI_BV:
    case BILIBILI_AV:
      res = bilibili_normal_parse_info (self, reader, info, error);
      break;
    case BILIBILI_BANGUMI_EP:
    case BILIBILI_BANGUMI_SS:
      res = bilibili_bangumi_parse_info (self, reader, info, error);
      break;
    default:
      break;
  }

  self->had_info = TRUE;

  g_object_unref (reader);

  return res;
}

static GtuberFlow
parse_media_streams (GtuberBilibili *self, JsonParser *parser,
    GtuberMediaInfo *info, GError **error)
{
  JsonReader *reader = json_reader_new (json_parser_get_root (parser));
  const gchar *obj_name = NULL;

  switch (self->bili_type) {
    case BILIBILI_BV:
    case BILIBILI_AV:
      obj_name = "data";
      break;
    case BILIBILI_BANGUMI_EP:
    case BILIBILI_BANGUMI_SS:
      obj_name = "result";
      break;
    default:
      g_assert_not_reached ();
  }

  if (json_reader_read_member (reader, obj_name)) {
    if (json_reader_read_member (reader, "dash")) {
      _add_dash_media_streams (self, reader, info, "video");
      _add_dash_media_streams (self, reader, info, "audio");
    }
    json_reader_end_member (reader);
  }
  json_reader_end_member (reader);

  g_object_unref (reader);

  if (*error)
    return GTUBER_FLOW_ERROR;

  return GTUBER_FLOW_OK;
}

static GtuberFlow
gtuber_bilibili_create_request (GtuberWebsite *website,
    GtuberMediaInfo *info, SoupMessage **msg, GError **error)
{
  GtuberBilibili *self = GTUBER_BILIBILI (website);
  SoupMessageHeaders *headers;
  gchar *uri_str = NULL;

  switch (self->bili_type) {
    case BILIBILI_BV:
    case BILIBILI_AV:
      uri_str = (!self->had_info)
          ? bilibili_normal_obtain_info_uri (self, _get_id_name (self))
          : bilibili_normal_obtain_media_uri (self);
      break;
    case BILIBILI_BANGUMI_EP:
    case BILIBILI_BANGUMI_SS:
      uri_str = (!self->had_info)
          ? bilibili_bangumi_obtain_info_uri (self, _get_id_name (self))
          : bilibili_bangumi_obtain_media_uri (self, _get_id_name (self));
      break;
    default:
      g_assert_not_reached ();
  }

  *msg = soup_message_new ("GET", uri_str);
  g_debug ("URI: %s", uri_str);
  g_free (uri_str);

  headers = (*msg)->request_headers;

  soup_message_headers_replace (headers,
      "Referer", gtuber_website_get_uri (website));

  return GTUBER_FLOW_OK;
}

static GtuberFlow
gtuber_bilibili_parse_input_stream (GtuberWebsite *website,
    GInputStream *stream, GtuberMediaInfo *info, GError **error)
{
  GtuberBilibili *self = GTUBER_BILIBILI (website);
  JsonParser *parser;
  GtuberFlow res = GTUBER_FLOW_OK;

  parser = json_parser_new ();
  json_parser_load_from_stream (parser, stream, NULL, error);
  if (*error)
    goto finish;

  gtuber_utils_json_parser_debug (parser);

  res = (!self->had_info)
      ? parse_info (self, parser, info, error)
      : parse_media_streams (self, parser, info, error);

finish:
  g_object_unref (parser);

  if (*error)
    return GTUBER_FLOW_ERROR;

  return res;
}

GtuberFlow
bilibili_get_flow_from_plugin_props (GtuberBilibili *self, GError **error)
{
  g_debug ("Has bvid: %s, aid: %i, cid: %i",
      self->bvid, self->aid, self->cid);

  /* We have info that we are going to use
   * to obtain streams in next step */
  if (self->bvid || self->aid || self->cid)
    return GTUBER_FLOW_RESTART;

  g_set_error (error, GTUBER_WEBSITE_ERROR,
      GTUBER_WEBSITE_ERROR_PARSE_FAILED,
      "Could not obtain required params");
  return GTUBER_FLOW_ERROR;
}

GtuberWebsite *
query_plugin (GUri *uri)
{
  GtuberBilibili *bilibili = NULL;
  const gchar *host, *path;
  gchar **parts;

  host = g_uri_get_host (uri);

  if (!g_str_has_suffix (host, "bilibili.com"))
    goto fail;

  /* FIXME: Support live streams */
  if (g_str_has_prefix (host, "live"))
    goto fail;

  path = g_uri_get_path (uri);
  if (!path)
    goto fail;

  g_debug ("URI path: %s", path);

  parts = g_strsplit (path, "/", 4);
  if (parts[1] && parts[2]) {
    BilibiliType bili_type = BILIBILI_UNKNOWN;
    const gchar *tmp_id = NULL;

    if (!strcmp (parts[1], "video")) {
      if (g_str_has_prefix (parts[2], "BV"))
        bili_type = BILIBILI_BV;
      else if (g_str_has_prefix (parts[2], "av"))
        bili_type = BILIBILI_AV;

      tmp_id = parts[2];
    } else if (parts[3] && !strcmp (parts[1], "bangumi")) {
      if (g_str_has_prefix (parts[3], "ep"))
        bili_type = BILIBILI_BANGUMI_EP;
      else if (g_str_has_prefix (parts[3], "ss"))
        bili_type = BILIBILI_BANGUMI_SS;

      tmp_id = parts[3];
    }

    if (bili_type != BILIBILI_UNKNOWN && tmp_id) {
      bilibili = g_object_new (GTUBER_TYPE_BILIBILI, NULL);
      bilibili->bili_type = bili_type;
      bilibili->video_id = g_strdup (tmp_id + 2);

      g_debug ("Requested type: %i, video: %s",
          bilibili->bili_type, bilibili->video_id);
    }
  }
  g_strfreev (parts);

  if (bilibili)
    return GTUBER_WEBSITE (bilibili);

fail:
  return NULL;
}