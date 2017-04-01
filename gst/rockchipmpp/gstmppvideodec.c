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

#include "gstmppdecbufferpool.h"
#include "gstmppvideodec.h"

GST_DEBUG_CATEGORY (mpp_video_dec_debug);
#define GST_CAT_DEFAULT mpp_video_dec_debug

#define parent_class gst_mpp_video_dec_parent_class
G_DEFINE_TYPE (GstMppVideoDec, gst_mpp_video_dec, GST_TYPE_VIDEO_DECODER);

#define NB_OUTPUT_BUFS 22       /* nb frames necessary for display pipeline */

/* GstVideoDecoder base class method */
static GstStaticPadTemplate gst_mpp_video_dec_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264,"
        "stream-format = (string) { byte-stream },"
        "alignment = (string) { au },"
        "parsed = (boolean) true"
        ";"
        "video/x-h265,"
        "stream-format = (string) { byte-stream },"
        "alignment = (string) { au },"
        "parsed = (boolean) true"
        ";"
        "video/mpeg,"
        "mpegversion = (int) { 1, 2, 4 },"
        "parsed = (boolean) true,"
        "systemstream = (boolean) false"
        ";" "video/x-vp8" ";" "video/x-vp9" ";")
    );

static GstStaticPadTemplate gst_mpp_video_dec_src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, "
        "format = (string) NV12, "
        "width  = (int) [ 32, 4096 ], " "height =  (int) [ 32, 4096 ]"
        ";"
        "video/x-raw, "
        "format = (string) NV16, "
        "width  = (int) [ 32, 4096 ], " "height =  (int) [ 32, 4096 ]"
        ";"
        "video/x-raw, "
        "format = (string) NV12_10LE40, "
        "width  = (int) [ 32, 4096 ], " "height =  (int) [ 32, 4096 ]" ";")
    );

static gboolean
gst_mpp_video_dec_close (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  if (self->mppobject) {
    gst_mpp_object_destroy (self->mppobject);
    self->mppobject = NULL;
  }

  GST_DEBUG_OBJECT (self, "Rockchip MPP object closed");

  return TRUE;
}

/* Open the device */
static gboolean
gst_mpp_video_dec_open (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  self->mppobject = gst_mpp_object_new (GST_ELEMENT (self), FALSE);
  if (!self->mppobject)
    return FALSE;

  GST_DEBUG_OBJECT (self, "created mpp object");
  return TRUE;
}

static gboolean
gst_mpp_video_dec_start (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Starting");
  gst_mpp_object_unlock (self->mppobject);
  g_atomic_int_set (&self->active, TRUE);
  self->output_flow = GST_FLOW_OK;

  return TRUE;
}

static gboolean
gst_mpp_video_dec_stop (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Stopping");

  /* Kill mpp output thread to stop */
  gst_mpp_object_unlock (self->mppobject);
  gst_mpp_object_flush (self->mppobject);
  /* Wait for mpp output thread to stop */
  gst_pad_stop_task (decoder->srcpad);

  GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  self->output_flow = GST_FLOW_OK;
  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

  /* Should have been flushed already */
  g_assert (g_atomic_int_get (&self->active) == FALSE);

  gst_mpp_object_destroy (self->mppobject);
  self->mppobject = NULL;

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);

  GST_DEBUG_OBJECT (self, "Stopped");

  return TRUE;
}

static gboolean
gst_mpp_video_dec_flush (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  gboolean ret = FALSE;

  ret = gst_mpp_object_flush (self->mppobject);
  /* Ensure the processing thread has stopped for the reverse playback
   * discount case */
  if (gst_pad_get_task_state (decoder->srcpad) == GST_TASK_STARTED) {
    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    gst_mpp_object_unlock (self->mppobject);
    gst_pad_stop_task (decoder->srcpad);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  }
  self->output_flow = GST_FLOW_OK;

  gst_mpp_object_unlock_stop (self->mppobject);
  return !ret;
}

static gboolean
gst_mpp_video_dec_finish (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;

  if (gst_pad_get_task_state (decoder->srcpad) != GST_TASK_STARTED)
    goto done;

  GST_DEBUG_OBJECT (self, "Finishing decoding");

  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
  if (gst_mpp_object_sendeos (self->mppobject)) {
    /* Wait for src thread to stop */
    GstTask *task = decoder->srcpad->task;
    GST_OBJECT_LOCK (task);
    while (GST_TASK_STATE (task) == GST_TASK_STARTED)
      GST_TASK_WAIT (task);
    GST_OBJECT_UNLOCK (task);
    ret = GST_FLOW_FLUSHING;
  }
  GST_VIDEO_DECODER_STREAM_LOCK (decoder);

  if (ret == GST_FLOW_FLUSHING)
    ret = self->output_flow;

  GST_DEBUG_OBJECT (self, "Finished");
done:
  return ret;
}

static gboolean
gst_mpp_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Setting format: %" GST_PTR_FORMAT, state->caps);

  if (self->input_state) {
    GstQuery *query = gst_query_new_drain ();

    if (gst_caps_is_strictly_equal (self->input_state->caps, state->caps))
      goto done;

    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;

    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    if (gst_mpp_object_sendeos (self->mppobject)) {
      /* Wait for mpp output thread to stop */
      GstTask *task = decoder->srcpad->task;
      if (task != NULL) {
        GST_OBJECT_LOCK (task);
        while (GST_TASK_STATE (task) == GST_TASK_STARTED)
          GST_TASK_WAIT (task);
        GST_OBJECT_UNLOCK (task);
      }
    }

    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
    /* Query the downstream to release buffers from buffer pool */
    if (!gst_pad_peer_query (GST_VIDEO_DECODER_SRC_PAD (self), query))
      GST_DEBUG_OBJECT (self, "drain query failed");
    gst_query_unref (query);

    gst_mpp_object_close_pool (self->mppobject);
    gst_mpp_object_init (self->mppobject, state->caps);

    self->output_flow = GST_FLOW_OK;
  } else {
    if (!gst_mpp_object_init (self->mppobject, state->caps))
      goto device_error;
  }

  self->input_state = gst_video_codec_state_ref (state);

done:
  return TRUE;

  /* Errors */
device_error:
  {
    GST_ERROR_OBJECT (self, "Failed to set format of mpp");
    return FALSE;
  }
}

static void
gst_mpp_video_dec_loop (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  GstBuffer *buffer;
  GstVideoCodecFrame *frame;
  GstBufferPool *pool;
  GstFlowReturn ret = GST_FLOW_OK;

  pool = self->mppobject->pool;
  ret = gst_buffer_pool_acquire_buffer (pool, &buffer, NULL);
  if (ret != GST_FLOW_OK && ret != GST_FLOW_CUSTOM_ERROR_1)
    goto beach;

  frame = gst_video_decoder_get_oldest_frame (decoder);
  if (frame) {
    if (ret == GST_FLOW_CUSTOM_ERROR_1) {
      gst_video_decoder_drop_frame (decoder, frame);
      return;
    }
    frame->output_buffer = buffer;

    buffer = NULL;
    ret = gst_video_decoder_finish_frame (decoder, frame);

    GST_TRACE_OBJECT (self, "finish buffer ts=%" GST_TIME_FORMAT,
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (frame->output_buffer)));

    if (ret != GST_FLOW_OK)
      goto beach;
  } else {
    GST_WARNING_OBJECT (self, "Decoder is producing too many buffers");
    gst_buffer_unref (buffer);
  }

  return;

beach:
  GST_DEBUG_OBJECT (self, "Leaving output thread: %s", gst_flow_get_name (ret));

  gst_buffer_replace (&buffer, NULL);
  self->output_flow = ret;
  gst_pad_pause_task (decoder->srcpad);
}

static GstFlowReturn
gst_mpp_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;
  gboolean processed = FALSE;
  GstBuffer *tmp;

  GST_DEBUG_OBJECT (self, "Handling frame %d", frame->system_frame_number);

  if (G_UNLIKELY (!g_atomic_int_get (&self->active)))
    goto flushing;

  if (G_UNLIKELY (!GST_MPP_IS_ACTIVE (self->mppobject))) {
    GstBuffer *codec_data;
    GstMppReturn ret = GST_MPP_OK;
    GstBufferPool *pool = NULL;

    codec_data = self->input_state->codec_data;
    if (codec_data) {
      gst_buffer_ref (codec_data);

      GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
      /* FIXME: send key stream */
      ret = gst_mpp_object_send_stream (self->mppobject, codec_data);
      if (ret != GST_MPP_OK)
        return GST_FLOW_ERROR;
      GST_VIDEO_DECODER_STREAM_LOCK (decoder);
      gst_buffer_unref (codec_data);
    }
    codec_data = gst_buffer_ref (frame->input_buffer);
    processed = TRUE;

    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    /* FIXME: send key stream */
    ret = gst_mpp_object_send_stream (self->mppobject, codec_data);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
    if (ret != GST_MPP_OK)
      return GST_FLOW_ERROR;
    gst_mpp_object_output_block (self->mppobject, -1L);

    ret = gst_mpp_object_acquire_output_format (self->mppobject);
    if (ret == GST_MPP_OK) {
      GstVideoCodecState *output_state;
      GstVideoInfo *info = &self->mppobject->info;
      GstStructure *config = gst_buffer_pool_get_config (pool);

      output_state =
          gst_video_decoder_set_output_state (decoder,
          info->finfo->format, info->width, info->height, self->input_state);
      output_state->info.interlace_mode = info->interlace_mode;
      gst_video_codec_state_unref (output_state);

      gst_mpp_object_setup_pool (self->mppobject);
      pool = GST_BUFFER_POOL (self->mppobject->pool);
      if (!pool)
        goto not_negotiated;

      /* NOTE: if you suffer from the reconstruction of buffer pool which slows
       * down the decoding, then don't allocate more than 10 buffers here */
      gst_buffer_pool_config_set_params (config, output_state->caps,
          self->mppobject->info.size, NB_OUTPUT_BUFS, NB_OUTPUT_BUFS);
      gst_buffer_pool_config_set_video_alignment (config,
          &self->mppobject->align);

      if (!gst_buffer_pool_set_config (pool, config))
        goto error_activate_pool;
      /* activate the pool: the buffers are allocated */
      if (gst_buffer_pool_set_active (self->mppobject->pool, TRUE) == FALSE)
        goto error_activate_pool;

      gst_mpp_object_info_change (self->mppobject);
    } else {
      goto not_negotiated;
    }
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  }

  /* Start the output thread if it is not started before */
  task_state = gst_pad_get_task_state (GST_VIDEO_DECODER_SRC_PAD (self));
  if (task_state == GST_TASK_STOPPED || task_state == GST_TASK_PAUSED) {
    /* It's possible that the processing thread stopped due to an error */
    if (self->output_flow != GST_FLOW_OK &&
        self->output_flow != GST_FLOW_FLUSHING) {
      GST_DEBUG_OBJECT (self, "Processing loop stopped with error, leaving");
      ret = self->output_flow;
      goto drop;
    }

    GST_DEBUG_OBJECT (self, "Starting decoding thread");

    self->output_flow = GST_FLOW_FLUSHING;
    if (!gst_pad_start_task (decoder->srcpad,
            (GstTaskFunction) gst_mpp_video_dec_loop, self, NULL))
      goto start_task_failed;
  }

  if (!processed) {
    GstMppReturn ret = GST_MPP_OK;
  retry:
    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    ret = gst_mpp_object_send_stream (self->mppobject, frame->input_buffer);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);

    if (GST_MPP_BUSY == ret) {
      if (gst_pad_get_task_state (GST_VIDEO_DECODER_SRC_PAD (self)) !=
          GST_TASK_STARTED) {
        ret = self->output_flow;
        goto drop;
      }
      goto retry;
    } else if (ret != GST_MPP_OK) {
      return GST_FLOW_ERROR;
    }

    /* No need to keep input arround */
    gst_buffer_replace (&frame->input_buffer, NULL);
  }

  /* No need to keep input arround */
  tmp = frame->input_buffer;
  frame->input_buffer = gst_buffer_new ();
  gst_buffer_copy_into (frame->input_buffer, tmp,
      GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS |
      GST_BUFFER_COPY_META, 0, 0);
  gst_buffer_unref (tmp);

  gst_video_codec_frame_unref (frame);
  return ret;

  /* ERRORS */
error_activate_pool:
  {
    GST_ERROR_OBJECT (self, "Unable to activate the pool");
    ret = GST_FLOW_ERROR;
    goto drop;
  }
flushing:
  {
    ret = GST_FLOW_FLUSHING;
    goto drop;
  }
start_task_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        ("Failed to start decoding thread."), (NULL));
    ret = GST_FLOW_ERROR;
    goto drop;
  }
not_negotiated:
  {
    GST_ERROR_OBJECT (self, "not negotiated");
    gst_video_decoder_drop_frame (decoder, frame);
    return GST_FLOW_NOT_NEGOTIATED;
  }
drop:
  {
    GST_ERROR_OBJECT (self, "can't process this frame");
    gst_video_decoder_drop_frame (decoder, frame);
    return ret;
  }
}

static gboolean
gst_mpp_video_dec_sink_event (GstVideoDecoder * decoder, GstEvent * event)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  gboolean ret;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (self, "flush start");
      gst_mpp_object_sendeos (self->mppobject);
      break;
    default:
      break;
  }

  ret = GST_VIDEO_DECODER_CLASS (parent_class)->sink_event (decoder, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      gst_pad_stop_task (decoder->srcpad);
      GST_DEBUG_OBJECT (self, "flush done");
      break;
    default:
      break;
  }

  return ret;
}

static GstStateChangeReturn
gst_mpp_video_dec_change_state (GstElement * element, GstStateChange transition)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (element);
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    g_atomic_int_set (&self->active, FALSE);
    gst_mpp_object_sendeos (self->mppobject);
    gst_mpp_object_unlock (self->mppobject);
    gst_pad_stop_task (decoder->srcpad);
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_mpp_video_dec_class_init (GstMppVideoDecClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mpp_video_dec_src_template));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mpp_video_dec_sink_template));

  video_decoder_class->open = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_open);
  video_decoder_class->close = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_close);
  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_stop);
  video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_finish);
  video_decoder_class->set_format = GST_DEBUG_FUNCPTR
      (gst_mpp_video_dec_set_format);
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_mpp_video_dec_handle_frame);
  video_decoder_class->flush = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_flush);
  video_decoder_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_mpp_video_dec_sink_event);

  element_class->change_state = GST_DEBUG_FUNCPTR
      (gst_mpp_video_dec_change_state);

  GST_DEBUG_CATEGORY_INIT (mpp_video_dec_debug, "mppvideodec", 0,
      "mpp video decoder");

  gst_element_class_set_static_metadata (element_class,
      "Rockchip's MPP video decoder", "Decoder/Video",
      "Multicodec (HEVC / AVC / VP8 / VP9) hardware decoder",
      "Randy Li <randy.li@rock-chips.com>");
}

static void
gst_mpp_video_dec_init (GstMppVideoDec * self)
{
  GstVideoDecoder *decoder = (GstVideoDecoder *) self;

  gst_video_decoder_set_packetized (decoder, TRUE);

  self->input_state = NULL;
  self->mppobject = NULL;
}
