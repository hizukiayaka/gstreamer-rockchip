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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <gst/gst.h>

#include "gstmppobject.h"
#include "gstmppdecbufferpool.h"

GST_DEBUG_CATEGORY_EXTERN (mpp_debug);
#define GST_CAT_DEFAULT mpp_debug

#define parent_class gst_mpp_object_parent_class

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

static GstVideoFormat
mpp_frame_type_to_gst_video_format (MppFrameFormat fmt)
{
  switch (fmt) {
    case MPP_FMT_YUV420SP:
      return GST_VIDEO_FORMAT_NV12;
      break;
    case MPP_FMT_YUV420SP_10BIT:
      return GST_VIDEO_FORMAT_P010_10LEC;
      break;
    case MPP_FMT_YUV422SP:
      return GST_VIDEO_FORMAT_NV16;
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
gst_mpp_video_frame_to_format (MppFrame mframe, GstVideoInfo * info,
    GstVideoAlignment * align)
{
  gint hor_stride, ver_stride, mv_size;
  GstVideoFormat format;
  GstVideoInterlaceMode mode;

  if (NULL == mframe || NULL == info || NULL == align)
    return FALSE;
  gst_video_info_init (info);
  gst_video_alignment_reset (align);

  format = mpp_frame_type_to_gst_video_format (mpp_frame_get_fmt (mframe));
  if (format == GST_VIDEO_FORMAT_UNKNOWN)
    return FALSE;
  mode = mpp_frame_get_mode (mframe);

  info->finfo = gst_video_format_get_info (format);
  info->width = mpp_frame_get_width (mframe);
  info->height = mpp_frame_get_height (mframe);
  info->interlace_mode = mpp_frame_mode_to_gst_interlace_mode (mode);

  hor_stride = mpp_frame_get_hor_stride (mframe);
  ver_stride = mpp_frame_get_ver_stride (mframe);
  for (guint i = 0; i < GST_VIDEO_INFO_N_PLANES (info); i++) {
    /* FIXME only work for simple pixel format */
    info->stride[i] = hor_stride;
    if (i == 0)
      info->offset[0] = 0;
    else
      info->offset[i] = info->offset[i - 1] + hor_stride * ver_stride;
  }

  for (guint i = 0; i < GST_VIDEO_INFO_N_COMPONENTS (info); i++)
    info->size +=
        GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (info->finfo, i, info->stride[i])
        * GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (info->finfo, i, ver_stride);
  mv_size = info->size * 2 / 6;
  info->size += mv_size;

  align->padding_right = hor_stride - info->width;
  align->padding_bottom = ver_stride - info->height;

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

  gst_mpp_object_close_pool (self);

  if (self->mpp_ctx != NULL) {
    mpp_destroy (self->mpp_ctx);
    self->mpp_ctx = NULL;
  }
  self->active = FALSE;

  GST_DEBUG_OBJECT (self, "Rockchip MPP context closed");

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

  self->type = is_encoder ? MPP_CTX_ENC : MPP_CTX_DEC;

  mpp_create (&self->mpp_ctx, &self->mpi);

  self->active = FALSE;
  self->pool = NULL;

  return self;
}

gboolean
gst_mpp_object_init (GstMppObject * self, GstCaps * caps)
{
  const GstStructure *structure;
  MppCodingType codingtype;

  structure = gst_caps_get_structure (caps, 0);
  codingtype = to_mpp_codec (structure);
  if (MPP_VIDEO_CodingUnused == codingtype)
    goto format_error;

  if (mpp_init (self->mpp_ctx, self->type, codingtype))
    goto mpp_init_error;

  self->active = FALSE;
  self->pool = NULL;

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
    g_free (self);
    return FALSE;
  }
}

gboolean
gst_mpp_object_sendeos (GstMppObject * self)
{
  if (self->type == MPP_CTX_DEC) {
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
  if (self->type == MPP_CTX_DEC) {
    MppFrame frame = NULL;
    ret = self->mpi->decode_get_frame (self->mpp_ctx, &frame);
    if (ret == MPP_ERR_TIMEOUT)
      return GST_MPP_BUSY;
    if (ret || NULL == frame) {
      GST_ERROR_OBJECT (self, "can't get valid info %d", ret);
      return GST_MPP_ERROR;
    }

    if (gst_mpp_video_frame_to_format (frame, &self->info, &self->align))
      return GST_MPP_OK;
  }
  return GST_MPP_ERROR;
}

inline void
gst_mpp_object_info_change (GstMppObject * self)
{
  if (self->type == MPP_CTX_DEC)
    self->mpi->control (self->mpp_ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
}

inline void
gst_mpp_object_output_block (GstMppObject * self, gint64 timeout)
{
  gint block_flag = MPP_POLL_BLOCK;

  self->mpi->control (self->mpp_ctx, MPP_SET_OUTPUT_BLOCK,
      (gpointer) & block_flag);
  self->mpi->control (self->mpp_ctx, MPP_SET_OUTPUT_BLOCK_TIMEOUT,
      (gpointer) & timeout);
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

void
gst_mpp_object_setup_pool (GstMppObject * self)
{
  if (self->type == MPP_CTX_DEC) {
    self->pool = gst_mpp_dec_buffer_pool_new (self, NULL);
  }
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

/* Reference by bufferpool */
GstFlowReturn
gst_mpp_object_fetch_dec_index (GstMppObject * self, gint * out_index)
{
  MppFrame frame = NULL;
  MppBuffer mpp_buf;
  MPP_RET ret = MPP_NOK;
  gint index;

  if (out_index == NULL)
    return GST_FLOW_ERROR;

  ret = self->mpi->decode_get_frame (self->mpp_ctx, &frame);
  if (ret != MPP_SUCCESS || NULL == frame)
    goto mpp_error;

  if (mpp_frame_get_discard (frame) || mpp_frame_get_errinfo (frame))
    goto drop_frame;
  /* get from the pool the GstBuffer associated with the index */
  mpp_buf = mpp_frame_get_buffer (frame);
  if (NULL == mpp_buf)
    goto eos;
  index = mpp_buffer_get_index (mpp_buf);

  if (index < 0)
    goto no_buffer;

  /*
   * Increase the reference of the buffer or the destroy the mpp frame
   * would decrease the reference and put it back to unused status
   */
  mpp_buffer_inc_ref (mpp_buf);
  mpp_frame_deinit (&frame);
  *out_index = index;

  return GST_FLOW_OK;

eos:
  {
    GST_INFO_OBJECT (self, "got eos or %d", ret);
    return GST_FLOW_EOS;
  }
  /* ERRORS */
mpp_error:
  {
    GST_ERROR_OBJECT (self, "mpp error %d", ret);
    return GST_FLOW_ERROR;
  }
no_buffer:
  {
    GST_ERROR_OBJECT (self, "No free buffer found in the pool at index %d",
        index);
    return GST_FLOW_ERROR;
  }
drop_frame:
  {
    mpp_frame_deinit (&frame);
    return GST_FLOW_CUSTOM_ERROR_1;
  }
}

/* Reference by bufferpool */
void
gst_mpp_object_config_pool (GstMppObject * self, gpointer pool)
{
  if (self->type == MPP_CTX_DEC) {
    self->mpi->control (self->mpp_ctx, MPP_DEC_SET_EXT_BUF_GROUP, pool);
  }
}
