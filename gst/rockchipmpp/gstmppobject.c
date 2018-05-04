/*
 * Copyright 2017 Rockchip Electronics Co., Ltd
 *     Author: Randy Li <randy.li@rock-chips.com>
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
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/gst-i18n-plugin.h>

#include "gstmppobject.h"
#include "gstmppbufferpool.h"

GST_DEBUG_CATEGORY_EXTERN (mpp_debug);
#define GST_CAT_DEFAULT mpp_debug

#define parent_class gst_mpp_object_parent_class

#define DEFAULT_PROP_IO_MODE            GST_MPP_IO_ION

#define GST_MPP_FLOW_FRAME_ERROR	GST_FLOW_CUSTOM_ERROR
#define GST_MPP_FLOW_FRAME_DISCARD	GST_FLOW_CUSTOM_ERROR_1

enum
{
  PROP_0,
  MPP_STD_OBJECT_PROPS,
};

static MppCodingType
to_mpp_codec (const GstStructure * s)
{
  if (gst_structure_has_name (s, "video/x-h264"))
    return MPP_VIDEO_CodingAVC;

  if (gst_structure_has_name (s, "video/x-h265"))
    return MPP_VIDEO_CodingHEVC;

  if (gst_structure_has_name (s, "video/x-h263"))
    return MPP_VIDEO_CodingH263;

  if (gst_structure_has_name (s, "video/mpeg")) {
    gint mpegversion = 0;
    if (gst_structure_get_int (s, "mpegversion", &mpegversion)) {
      switch (mpegversion) {
        case 1:
        case 2:
          return MPP_VIDEO_CodingMPEG2;
          break;
        case 4:
          return MPP_VIDEO_CodingMPEG4;
          break;
        default:
          break;
      }
    }
  }

  if (gst_structure_has_name (s, "video/x-vp8"))
    return MPP_VIDEO_CodingVP8;
  if (gst_structure_has_name (s, "video/x-vp9"))
    return MPP_VIDEO_CodingVP9;

  /* add more type here */
  return MPP_VIDEO_CodingUnused;
}

static MppFrameFormat
to_mpp_pixel (GstCaps * caps, GstVideoInfo * info)
{
  GstStructure *structure;
  const gchar *mimetype;

  structure = gst_caps_get_structure (caps, 0);
  mimetype = gst_structure_get_name (structure);

  if (g_str_equal (mimetype, "video/x-raw")) {
    switch (GST_VIDEO_INFO_FORMAT (info)) {
      case GST_VIDEO_FORMAT_I420:
        return MPP_FMT_YUV420P;
        break;
      case GST_VIDEO_FORMAT_NV12:
        return MPP_FMT_YUV420SP;
        break;
      case GST_VIDEO_FORMAT_YUY2:
        return MPP_FMT_YUV422_YUYV;
        break;
      case GST_VIDEO_FORMAT_UYVY:
        return MPP_FMT_YUV422_UYVY;
        break;
      default:
        break;
    }
  }
  return MPP_FMT_BUTT;
}

static GstVideoFormat
mpp_frame_type_to_gst_video_format (MppFrameFormat fmt)
{
  switch (fmt) {
    case MPP_FMT_YUV420SP:
      return GST_VIDEO_FORMAT_NV12;
      break;
    case MPP_FMT_YUV420SP_10BIT:
      return GST_VIDEO_FORMAT_NV12_10LE40;
      break;
    case MPP_FMT_YUV422SP:
      return GST_VIDEO_FORMAT_NV16;
      break;
    case MPP_FMT_YUV420P:
      return GST_VIDEO_FORMAT_I420;
      break;
    case MPP_FMT_YUV422_YUYV:
      return GST_VIDEO_FORMAT_YUY2;
      break;
    case MPP_FMT_YUV422_UYVY:
      return GST_VIDEO_FORMAT_UYVY;
      break;
    default:
      return GST_VIDEO_FORMAT_UNKNOWN;
      break;
  }
  return GST_VIDEO_FORMAT_UNKNOWN;
}

static GstVideoInterlaceMode
mpp_frame_mode_to_gst_interlace_mode (RK_U32 mode)
{
  switch (mode & MPP_FRAME_FLAG_FIELD_ORDER_MASK) {
    case MPP_FRAME_FLAG_DEINTERLACED:
      return GST_VIDEO_INTERLACE_MODE_MIXED;
    case MPP_FRAME_FLAG_BOT_FIRST:
    case MPP_FRAME_FLAG_TOP_FIRST:
      return GST_VIDEO_INTERLACE_MODE_INTERLEAVED;
    default:
      return GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
  }
}

static gboolean
gst_mpp_video_frame_to_info (MppFrame mframe, GstVideoInfo * info,
    GstVideoInfo * align_info)
{
  gsize hor_stride, ver_stride;
  gsize width, height, mv_size, cr_h;
  GstVideoFormat format;
  GstVideoInterlaceMode mode;

  if (NULL == mframe || NULL == info)
    return FALSE;

  format = mpp_frame_type_to_gst_video_format (mpp_frame_get_fmt (mframe));
  if (format == GST_VIDEO_FORMAT_UNKNOWN)
    return FALSE;
  mode = mpp_frame_get_mode (mframe);
  width = mpp_frame_get_width (mframe);
  height = mpp_frame_get_height (mframe);

  gst_video_info_init (info);
  gst_video_info_set_format (info, format, width, height);
  info->interlace_mode = mpp_frame_mode_to_gst_interlace_mode (mode);

  hor_stride = mpp_frame_get_hor_stride (mframe);
  ver_stride = mpp_frame_get_ver_stride (mframe);

  switch (info->finfo->format) {
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21:
    case GST_VIDEO_FORMAT_NV12_10LE40:
      info->stride[0] = hor_stride;
      info->stride[1] = hor_stride;
      info->offset[0] = 0;
      info->offset[1] = hor_stride * ver_stride;
      cr_h = GST_ROUND_UP_2 (ver_stride) / 2;
      info->size = info->offset[1] + info->stride[0] * cr_h;
      mv_size = info->size / 3;
      info->size += mv_size;
      break;
    default:
      g_assert_not_reached ();
      return FALSE;
  }

  if (align_info) {
    gst_video_info_init (align_info);
    switch (info->finfo->format) {
      case GST_VIDEO_FORMAT_NV12_10LE40:
        gst_video_info_set_format (align_info, format, (hor_stride / 10) << 3,
            ver_stride);
        break;
      default:
        gst_video_info_set_format (align_info, format, hor_stride, ver_stride);
        break;
    }
    align_info->size = info->size;
  }

  return TRUE;
}

GType
gst_mpp_io_mode_get_type (void)
{
  static GType mpp_io_mode = 0;

  if (!mpp_io_mode) {
    static const GEnumValue io_modes[] = {
      {GST_MPP_IO_AUTO, "GST_MPP_IO_AUTO", "auto"},
      {GST_MPP_IO_ION, "GST_MPP_IO_ION", "ion"},
      {GST_MPP_IO_DRMBUF, "GST_MPP_IO_DRMBUF", "drmbuf"},
      {GST_MPP_IO_DMABUF_IMPORT, "GST_MPP_IO_DMABUF_IMPORT", "dmabuf-import"},
      {GST_MPP_IO_RW, "GST_MPP_IO_RW", "internal copy"},
      {GST_MPP_IO_USERPTR, "GST_MPP_IO_USERPTR", "userptr"},
      {0, NULL, NULL}
    };
    mpp_io_mode = g_enum_register_static ("GstMppIOMode", io_modes);
  }
  return mpp_io_mode;
}

void
gst_mpp_object_install_properties_helper (GObjectClass * gobject_class)
{
  g_object_class_install_property (gobject_class, PROP_IO_MODE,
      g_param_spec_enum ("io-mode", "IO mode",
          "I/O mode",
          GST_TYPE_MPP_IO_MODE, DEFAULT_PROP_IO_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

gboolean
gst_mpp_object_get_property_helper (GstMppObject * self,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    case PROP_IO_MODE:
      g_value_set_enum (value, self->req_mode);
      break;
    default:
      return FALSE;
      break;
  }
  return TRUE;
}

gboolean
gst_mpp_object_set_property_helper (GstMppObject * self,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    case PROP_IO_MODE:
      self->req_mode = g_value_get_enum (value);
      break;
    default:
      return FALSE;
      break;
  }
  return TRUE;
}

/* Only the decoder request that */
void
gst_mpp_object_unlock (GstMppObject * self)
{
  if (self->pool && gst_buffer_pool_is_active (self->pool))
    gst_buffer_pool_set_flushing (self->pool, TRUE);
}

void
gst_mpp_object_unlock_stop (GstMppObject * self)
{
  if (self->pool && gst_buffer_pool_is_active (self->pool))
    gst_buffer_pool_set_flushing (self->pool, FALSE);
}

gboolean
gst_mpp_object_destroy (GstMppObject * self)
{

  g_return_val_if_fail (self != NULL, FALSE);
  gst_mpp_object_close_pool (self);

  if (self->mpp_ctx && self->element) {
    mpp_destroy (self->mpp_ctx);
    self->mpp_ctx = NULL;
  }
  GST_MPP_SET_INACTIVE (self);

  GST_DEBUG_OBJECT (self, "Rockchip MPP context closed");
  g_free (self);

  return TRUE;
}

/* create a new mpp/mpi instance */
GstMppObject *
gst_mpp_object_new (GstElement * element, gboolean is_encoder)
{
  GstMppObject *self;

  self = g_new0 (GstMppObject, 1);
  if (!self)
    return NULL;
  self->element = element;

  self->type = is_encoder ? GST_MPP_ENC_INPUT : GST_MPP_DEC_INPUT;
  self->mode = GST_MPP_IO_AUTO;
  self->req_mode = GST_MPP_IO_AUTO;

  switch (self->type) {
    case GST_MPP_DEC_INPUT:
      /* FIXME */
      self->req_mode = GST_MPP_IO_RW;
      break;
    case GST_MPP_DEC_OUTPUT:
      self->need_video_meta = TRUE;
      break;
    default:
      self->req_mode = GST_MPP_IO_AUTO;
      break;
  }

  self->mpp_ctx = NULL;
  self->mpi = NULL;
  self->active = FALSE;
  self->pool = NULL;

  return self;
}

gboolean
gst_mpp_object_open (GstMppObject * self)
{
  return mpp_create (&self->mpp_ctx, &self->mpi);
}

GstMppObject *
gst_mpp_object_open_shared (GstMppObject * self, GstMppObject * other)
{
  self->mpp_ctx = other->mpp_ctx;
  self->mpi = other->mpi;
  self->type = GST_MPP_DEC_OUTPUT;
  self->need_video_meta = TRUE;
  self->req_mode = GST_MPP_IO_AUTO;

  return self;
}

gboolean
gst_mpp_object_set_fmt (GstMppObject * self, GstCaps * caps)
{
  const GstStructure *structure;
  MppCodingType codingtype;

  structure = gst_caps_get_structure (caps, 0);
  codingtype = to_mpp_codec (structure);
  if (MPP_VIDEO_CodingUnused == codingtype)
    goto format_error;

  switch (self->type) {
    case GST_MPP_DEC_INPUT:
    case GST_MPP_DEC_OUTPUT:
      if (mpp_init (self->mpp_ctx, MPP_CTX_DEC, codingtype))
        goto mpp_init_error;
      break;
    case GST_MPP_ENC_INPUT:
    case GST_MPP_ENC_OUTPUT:
      if (mpp_init (self->mpp_ctx, MPP_CTX_ENC, codingtype))
        goto mpp_init_error;
      break;
  }

  return TRUE;
  /* Errors */
format_error:
  {
    GST_ERROR_OBJECT (self, "Unsupported format in caps: %" GST_PTR_FORMAT,
        caps);
  }
mpp_init_error:
  {
    GST_ERROR_OBJECT (self, "rockchip context init failed");
    if (!self->mpp_ctx)
      mpp_destroy (self->mpp_ctx);
    return FALSE;
  }
}

gboolean
gst_mpp_object_sendeos (GstMppObject * self)
{
  if (self->type == GST_MPP_DEC_INPUT) {
    MppPacket mpkt;

    mpp_packet_init (&mpkt, NULL, 0);
    mpp_packet_set_eos (mpkt);

    self->mpi->decode_put_packet (self->mpp_ctx, mpkt);
    mpp_packet_deinit (&mpkt);
  }

  return TRUE;
}

inline gboolean
gst_mpp_object_flush (GstMppObject * self)
{
  return self->mpi->reset (self->mpp_ctx);
}

GstMppReturn
gst_mpp_object_acquire_output_format (GstMppObject * self)
{
  MPP_RET ret;
  if (self->type == GST_MPP_DEC_OUTPUT) {
    MppFrame frame = NULL;
    ret = self->mpi->decode_get_frame (self->mpp_ctx, &frame);
    if (ret == MPP_ERR_TIMEOUT)
      return GST_MPP_BUSY;
    if (ret || NULL == frame) {
      GST_ERROR_OBJECT (self, "can't get valid info %d", ret);
      return GST_MPP_ERROR;
    }

    if (gst_mpp_video_frame_to_info (frame, &self->info, &self->align_info))
      return GST_MPP_OK;
  }
  return GST_MPP_ERROR;
}

inline void
gst_mpp_object_info_change (GstMppObject * self)
{
  if (self->type == GST_MPP_DEC_OUTPUT)
    self->mpi->control (self->mpp_ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
}

inline gboolean
gst_mpp_object_timeout (GstMppObject * self, gint64 timeout)
{
  MPP_RET ret = MPP_NOK;
  switch (self->type) {
    case GST_MPP_DEC_INPUT:
      self->mpi->control (self->mpp_ctx, MPP_SET_INPUT_TIMEOUT,
          (gpointer) & timeout);
      break;
    case GST_MPP_DEC_OUTPUT:
      self->mpi->control (self->mpp_ctx, MPP_SET_OUTPUT_TIMEOUT,
          (gpointer) & timeout);
      break;
    default:
      GST_ERROR_OBJECT (self, "can't set timeout for  %d", self->type);
      break;
  }

  return (ret == MPP_SUCCESS) ? TRUE : FALSE;
}

GstMppReturn
gst_mpp_object_send_stream (GstMppObject * self, GstBuffer * data)
{
  GstMapInfo mapinfo = GST_MAP_INFO_INIT;
  MppPacket mpkt = NULL;
  MPP_RET ret = MPP_NOK;

  gst_buffer_map (data, &mapinfo, GST_MAP_READ);
  mpp_packet_init (&mpkt, mapinfo.data, mapinfo.size);

  ret = self->mpi->decode_put_packet (self->mpp_ctx, mpkt);

  gst_buffer_unmap (data, &mapinfo);
  mpp_packet_deinit (&mpkt);

  if (ret == MPP_ERR_BUFFER_FULL)
    return GST_MPP_BUSY;
  if (ret != MPP_SUCCESS)
    goto send_stream_error;

  return GST_MPP_OK;
  /* ERROR */
send_stream_error:
  {
    GST_ERROR_OBJECT (self, "send packet failed %d", ret);
    return GST_MPP_ERROR;
  }
}

gboolean
gst_mpp_object_setup_pool (GstMppObject * self, GstCaps * caps)
{
  /* TODO */
  GstMppIOMode mode;

  mode = self->req_mode;

  if (mode == GST_MPP_IO_AUTO)
    mode = GST_MPP_IO_DRMBUF;

  self->mode = mode;
  /* Different the input and output of the decoder */
  switch (self->type) {
    case GST_MPP_DEC_INPUT:
      self->pool = gst_mpp_buffer_pool_new (self, caps);
      self->min_buffers = 0;
      break;
    case GST_MPP_DEC_OUTPUT:
      self->pool = gst_mpp_buffer_pool_new (self, caps);
      break;
    default:
      return FALSE;
  }

  GST_MPP_SET_ACTIVE (self);
  return TRUE;
}

void
gst_mpp_object_close_pool (GstMppObject * self)
{
  if (self->pool != NULL) {
    self->mpi->reset (self->mpp_ctx);
    gst_object_unref (self->pool);
    self->pool = NULL;
  }
  self->active = FALSE;
}

/* bufferpool would use this function */
GstFlowReturn
gst_mpp_object_dec_frame (GstMppObject * self, MppFrame * out_frame)
{
  MppFrame frame = NULL;
  gint ret;

  if (!self || !out_frame)
    return GST_FLOW_ERROR;

  ret = self->mpi->decode_get_frame (self->mpp_ctx, &frame);
  if (ret || !frame)
    goto mpp_error;

  *out_frame = frame;
  return GST_FLOW_OK;
  /* ERRORS */
mpp_error:
  {
    GST_ERROR_OBJECT (self, "mpp error %d", ret);
    return GST_FLOW_ERROR;
  }
}

/* bufferpool would use this function */
void
gst_mpp_object_config_pool (GstMppObject * self, gpointer pool)
{
  switch (self->type) {
    case GST_MPP_DEC_OUTPUT:
      self->mpi->control (self->mpp_ctx, MPP_DEC_SET_EXT_BUF_GROUP, pool);
      break;
    case GST_MPP_DEC_INPUT:
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}

gboolean
gst_mpp_object_decide_allocation (GstMppObject * obj, GstQuery * query)
{
  GstCaps *caps;
  GstBufferPool *pool = NULL, *other_pool = NULL;
  GstStructure *config;
  guint size, min, max, own_min = 0;
  gboolean update;
  gboolean has_video_meta;
  gboolean can_share_own_pool, pushing_from_our_pool = FALSE;
  GstAllocator *allocator = NULL;
  GstAllocationParams params = { 0 };

  GST_DEBUG_OBJECT (obj->element, "decide allocation");

#if 0
  /* FIXME */
  g_return_val_if_fail (obj->type == V4L2_BUF_TYPE_VIDEO_CAPTURE ||
      obj->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, FALSE);
#endif

  gst_query_parse_allocation (query, &caps, NULL);

  if (obj->pool == NULL) {
    if (!gst_mpp_object_setup_pool (obj, caps))
      goto pool_failed;
  }

  if (gst_query_get_n_allocation_params (query) > 0)
    gst_query_parse_nth_allocation_param (query, 0, &allocator, &params);

  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);
    update = TRUE;
  } else {
    pool = NULL;
    min = max = 0;
    size = 0;
    update = FALSE;
  }

  GST_DEBUG_OBJECT (obj->element, "allocation: size:%u min:%u max:%u pool:%"
      GST_PTR_FORMAT, size, min, max, pool);

  has_video_meta =
      gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

  can_share_own_pool = (has_video_meta || !obj->need_video_meta);

  /* select a pool */
  switch (obj->mode) {
    case GST_MPP_IO_DMABUF_IMPORT:
      /* in importing mode, prefer our own pool, and pass the other pool to
       * our own, so it can serve itself */
      if (pool == NULL)
        goto no_downstream_pool;
      /* TODO */
      gst_mpp_buffer_pool_set_other_pool (GST_MPP_BUFFER_POOL (obj->pool),
          pool);
      other_pool = pool;
      gst_object_unref (pool);
      pool = gst_object_ref (obj->pool);
      size = obj->info.size;
      break;

    case GST_MPP_IO_ION:
    case GST_MPP_IO_DRMBUF:
      /* in streaming mode, prefer our own pool */
      /* Check if we can use it ... */
      if (can_share_own_pool) {
        if (pool)
          gst_object_unref (pool);
        pool = gst_object_ref (obj->pool);
        size = obj->info.size;
        GST_DEBUG_OBJECT (obj->element,
            "streaming mode: using our own pool %" GST_PTR_FORMAT, pool);
        pushing_from_our_pool = TRUE;
      } else if (pool) {
        /* TODO: FIXME: most of the downstream can use the DMAbuf */
        GST_DEBUG_OBJECT (obj->element,
            "streaming mode: copying to downstream pool %" GST_PTR_FORMAT,
            pool);
      } else {
        GST_DEBUG_OBJECT (obj->element,
            "streaming mode: no usable pool, copying to generic pool");
        size = MAX (size, obj->info.size);
      }
      break;
    case GST_MPP_IO_AUTO:
    default:
      GST_WARNING_OBJECT (obj->element, "unhandled mode");
      break;
  }

  if (size == 0)
    goto no_size;

  /* If pushing from our own pool, configure it with queried minimum,
   * otherwise use the minimum required
   */
  if (pushing_from_our_pool) {
    /* When pushing from our own pool, we need what downstream one, to be able
     * to fill the pipeline, the minimum required to decoder according to the
     * driver and 2 more, so we don't endup up with everything downstream or
     * held by the decoder.
     */
    own_min = min + 2;

    /* If no allocation parameters where provided, allow for a little more
     * buffers and enable copy threshold */
    if (!update) {
      own_min += 2;
#if 0
      /* FIXME */
      gst_v4l2_buffer_pool_copy_at_threshold (GST_V4L2_BUFFER_POOL (pool),
          TRUE);
    } else {
      /* FIXME */
      gst_v4l2_buffer_pool_copy_at_threshold (GST_V4L2_BUFFER_POOL (pool),
          FALSE);
#endif
    }

  } else {
    /* In this case we'll have to configure two buffer pool. For our buffer
     * pool, we'll need what the driver one, and one more, so we can dequeu */
    own_min = MAX (own_min, GST_MPP_MIN_BUFFERS);

    /* for the downstream pool, we keep what downstream wants, though ensure
     * at least a minimum if downstream didn't suggest anything (we are
     * expecting the base class to create a default one for the context) */
    min = MAX (min, GST_MPP_MIN_BUFFERS);

    /* To import we need the other pool to hold at least own_min */
    if (obj->pool == pool)
      min += own_min;
  }

  /* Request a bigger max, if one was suggested but it's too small */
  if (max != 0)
    max = MAX (min, max);

  /* First step, configure our own pool */
  config = gst_buffer_pool_get_config (obj->pool);

  if (obj->need_video_meta || has_video_meta) {
    GST_DEBUG_OBJECT (obj->element, "activate Video Meta");
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
  }

  gst_buffer_pool_config_set_allocator (config, allocator, &params);
  gst_buffer_pool_config_set_params (config, caps, size, own_min, 0);

  GST_DEBUG_OBJECT (obj->element, "setting own pool config to %"
      GST_PTR_FORMAT, config);

  /* Our pool often need to adjust the value */
  if (!gst_buffer_pool_set_config (obj->pool, config)) {
    config = gst_buffer_pool_get_config (obj->pool);

    GST_DEBUG_OBJECT (obj->element, "own pool config changed to %"
        GST_PTR_FORMAT, config);

    /* our pool will adjust the maximum buffer, which we are fine with */
    if (!gst_buffer_pool_set_config (obj->pool, config))
      goto config_failed;
  }

  /* Now configure the other pool if different */
  if (obj->pool != pool)
    other_pool = pool;

  if (other_pool) {
    config = gst_buffer_pool_get_config (other_pool);
    gst_buffer_pool_config_set_allocator (config, allocator, &params);
    gst_buffer_pool_config_set_params (config, caps, size, min, max);

    GST_DEBUG_OBJECT (obj->element, "setting other pool config to %"
        GST_PTR_FORMAT, config);

    /* if downstream supports video metadata, add this to the pool config */
    if (has_video_meta) {
      GST_DEBUG_OBJECT (obj->element, "activate Video Meta");
      gst_buffer_pool_config_add_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_META);
    }

    if (!gst_buffer_pool_set_config (other_pool, config)) {
      config = gst_buffer_pool_get_config (other_pool);

      if (!gst_buffer_pool_config_validate_params (config, caps, size, min,
              max)) {
        gst_structure_free (config);
        goto config_failed;
      }

      if (!gst_buffer_pool_set_config (other_pool, config))
        goto config_failed;
    }
  }

  if (pool) {
    /* For simplicity, simply read back the active configuration, so our base
     * class get the right information */
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_params (config, NULL, &size, &min, &max);
    gst_structure_free (config);
  }

  if (update)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);

  if (allocator)
    gst_object_unref (allocator);

  if (pool)
    gst_object_unref (pool);

  return TRUE;

pool_failed:
  {
    /* setup_pool already send the error */
    goto cleanup;
  }
config_failed:
  {
    GST_ELEMENT_ERROR (obj->element, RESOURCE, SETTINGS,
        (_("Failed to configure internal buffer pool.")), (NULL));
    goto cleanup;
  }
no_size:
  {
    GST_ELEMENT_ERROR (obj->element, RESOURCE, SETTINGS,
        (_("Video device did not suggest any buffer size.")), (NULL));
    goto cleanup;
  }
cleanup:
  {
    if (allocator)
      gst_object_unref (allocator);

    if (pool)
      gst_object_unref (pool);
    return FALSE;
  }
no_downstream_pool:
  {
    GST_ELEMENT_ERROR (obj->element, RESOURCE, SETTINGS,
        (_("No downstream pool to import from.")),
        ("When importing DMABUF or USERPTR, we need a pool to import from"));
    return FALSE;
  }
}
