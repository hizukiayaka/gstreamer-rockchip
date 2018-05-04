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

#include "gstmppobject.h"
#include "gstmppbufferpool.h"
#include "gstmppvideodec.h"

GST_DEBUG_CATEGORY (mpp_video_dec_debug);
#define GST_CAT_DEFAULT mpp_video_dec_debug

#define parent_class gst_mpp_video_dec_parent_class
G_DEFINE_TYPE (GstMppVideoDec, gst_mpp_video_dec, GST_TYPE_VIDEO_DECODER);

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

static void
gst_mpp_video_dec_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (object);

  switch (prop_id) {
    default:
      if (!gst_mpp_object_set_property_helper (self->mpp_output,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

static void
gst_mpp_video_dec_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (object);

  switch (prop_id) {
    default:
      if (!gst_mpp_object_get_property_helper (self->mpp_output,
              prop_id, value, pspec)) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
}

/* Open the device */
static gboolean
gst_mpp_video_dec_open (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  if (gst_mpp_object_open (self->mpp_input))
    return FALSE;

  gst_mpp_object_open_shared (self->mpp_output, self->mpp_input);

  GST_DEBUG_OBJECT (self, "created mpp object");
  return TRUE;
}

static gboolean
gst_mpp_video_dec_close (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  gst_mpp_object_destroy (self->mpp_output);
  gst_mpp_object_destroy (self->mpp_input);

  GST_DEBUG_OBJECT (self, "Rockchip MPP object closed");

  return TRUE;
}

static gboolean
gst_mpp_video_dec_start (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Starting");
  gst_mpp_object_unlock (self->mpp_input);
  gst_mpp_object_unlock (self->mpp_output);
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
  gst_mpp_object_unlock (self->mpp_output);
  gst_mpp_object_flush (self->mpp_output);
  /* Wait for mpp output thread to stop */
  gst_pad_stop_task (decoder->srcpad);

  GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  self->output_flow = GST_FLOW_OK;
  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

  /* Should have been flushed already */
  g_assert (g_atomic_int_get (&self->active) == FALSE);

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

  ret = gst_mpp_object_flush (self->mpp_output);
  /* Ensure the processing thread has stopped for the reverse playback
   * discount case */
  if (gst_pad_get_task_state (decoder->srcpad) == GST_TASK_STARTED) {
    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    gst_mpp_object_unlock (self->mpp_output);
    gst_pad_stop_task (decoder->srcpad);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  }
  self->output_flow = GST_FLOW_OK;

  gst_mpp_object_unlock_stop (self->mpp_output);
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
  if (gst_mpp_object_sendeos (self->mpp_input)) {
    /* Wait for src thread to stop */
    GstTask *task = GST_PAD_TASK (GST_VIDEO_DECODER_SRC_PAD (decoder));
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
    if (gst_mpp_object_sendeos (self->mpp_input)) {
      /* Wait for mpp output thread to stop */
      GstTask *task = GST_PAD_TASK (GST_VIDEO_DECODER_SRC_PAD (decoder));
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

    gst_mpp_object_close_pool (self->mpp_output);

    self->output_flow = GST_FLOW_OK;
  } else {
    if (!gst_mpp_object_set_fmt (self->mpp_input, state->caps))
      goto device_error;
    gst_mpp_object_setup_pool (self->mpp_input, state->caps);
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
  GstMppBufferPool *mpp_pool = NULL;
  GstBufferPool *pool;
  GstBuffer *buffer = NULL;
  GstVideoCodecFrame *frame;
  GstFlowReturn ret;

  GST_LOG_OBJECT (decoder, "Allocate output buffer");
  self->output_flow = GST_FLOW_OK;

  mpp_pool = GST_MPP_BUFFER_POOL (self->mpp_output->pool);

  pool = gst_video_decoder_get_buffer_pool (decoder);
  if (pool == NULL) {
    ret = GST_FLOW_FLUSHING;
    goto beach;
  }

  ret = gst_buffer_pool_acquire_buffer (pool, &buffer, NULL);
  g_object_unref (pool);

  if (ret != GST_FLOW_OK)
    goto beach;

  GST_LOG_OBJECT (decoder, "Process output buffer");
  ret = gst_mpp_buffer_pool_process (mpp_pool, &buffer);

  if (ret != GST_FLOW_OK && ret != GST_MPP_FLOW_CORRUPTED_BUFFER)
    goto beach;

  frame = gst_video_decoder_get_oldest_frame (decoder);
  if (frame) {
    if (ret == GST_MPP_FLOW_CORRUPTED_BUFFER) {
      gst_video_decoder_drop_frame (decoder, frame);
      return;
    }
    frame->output_buffer = buffer;

    buffer = NULL;
    /* FIXME */

    GST_TRACE_OBJECT (self, "finish buffer ts=%" GST_TIME_FORMAT,
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (frame->output_buffer)));

    ret = gst_video_decoder_finish_frame (decoder, frame);

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
  gst_mpp_object_unlock (self->mpp_input);
  gst_pad_pause_task (decoder->srcpad);
}

static gboolean
gst_mpp_video_dec_update_src_caps (GstMppVideoDec * self)
{
  GstVideoDecoder *const vdec = GST_VIDEO_DECODER (self);
  GstVideoCodecState *output_state;
  GstCaps *allocation_caps;
  GstVideoInfo *vi;
  GstVideoFormat format;
  GstClockTime latency;
  gint fps_d, fps_n;
  guint width, height;

  format = GST_MPP_PIXELFORMAT (self->mpp_output);
  width = GST_MPP_WIDTH (self->mpp_output);
  height = GST_MPP_HEIGHT (self->mpp_output);

  output_state = gst_video_decoder_set_output_state (vdec,
      format, width, height, self->input_state);
  output_state->info.interlace_mode = GST_MPP_INTERLACE_MODE (self->mpp_output);

  vi = &output_state->info;
  output_state->caps = gst_video_info_to_caps (vi);

  /* Allocation query is different from pad's caps */
  allocation_caps = NULL;
  if (GST_VIDEO_INFO_WIDTH (&self->mpp_output->align_info) != width
      || GST_VIDEO_INFO_HEIGHT (&self->mpp_output->align_info) != height) {
    const gchar *format_str = NULL;

    allocation_caps = gst_caps_copy (output_state->caps);
    format_str = gst_video_format_to_string (format);
    gst_caps_set_simple (allocation_caps,
        "width", G_TYPE_INT,
        GST_VIDEO_INFO_WIDTH (&self->mpp_output->align_info), "height",
        G_TYPE_INT, GST_VIDEO_INFO_HEIGHT (&self->mpp_output->align_info),
        "format", G_TYPE_STRING, format_str, NULL);
    GST_INFO_OBJECT (self, "new alloc caps = %" GST_PTR_FORMAT,
        allocation_caps);
  }
  gst_caps_replace (&output_state->allocation_caps, allocation_caps);
  if (allocation_caps)
    gst_caps_unref (allocation_caps);

  GST_INFO_OBJECT (self, "new src caps = %" GST_PTR_FORMAT, output_state->caps);

  gst_video_codec_state_unref (output_state);

  fps_n = GST_VIDEO_INFO_FPS_N (vi);
  fps_d = GST_VIDEO_INFO_FPS_D (vi);
  if (fps_n <= 0 || fps_d <= 0) {
    GST_DEBUG_OBJECT (self, "forcing 25/1 framerate for latency calculation");
    fps_n = 25;
    fps_d = 1;
  }

  latency = gst_util_uint64_scale (2 * GST_SECOND, fps_d, fps_n);
  gst_video_decoder_set_latency (vdec, latency, latency);

  return TRUE;
}

static gboolean
gst_mpp_video_dec_src_negotiate (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  if (!gst_mpp_video_dec_update_src_caps (self))
    return FALSE;
  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

  if (!gst_video_decoder_negotiate (decoder))
    return FALSE;

  return TRUE;
}

static GstFlowReturn
gst_mpp_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;
  gboolean processed = FALSE;
  GstBuffer *tmp;
  GstTaskState task_state;

  GST_DEBUG_OBJECT (self, "Handling frame %d", frame->system_frame_number);

  if (G_UNLIKELY (!g_atomic_int_get (&self->active)))
    goto flushing;

  if (G_UNLIKELY (!GST_MPP_IS_ACTIVE (self->mpp_output))) {
    GstBuffer *codec_data;
    GstBufferPool *pool = NULL;

    GST_DEBUG_OBJECT (self, "Sending header");

    codec_data = self->input_state->codec_data;
    if (codec_data) {
      gst_buffer_ref (codec_data);
    } else {
      codec_data = gst_buffer_ref (frame->input_buffer);
      processed = TRUE;
    }

    pool = self->mpp_input->pool;
    /* Ensure input internal pool is active */
    if (!gst_buffer_pool_is_active (pool)) {
      GstStructure *config = gst_buffer_pool_get_config (pool);
      gst_buffer_pool_config_set_params (config, self->input_state->caps,
          /* FIXME */ 4096000, 2, 2);

      /* There is no reason to refuse this config */
      if (!gst_buffer_pool_set_config (pool, config))
        goto activate_failed;
      if (!gst_buffer_pool_set_active (pool, TRUE))
        goto activate_failed;
    }
    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    /* FIXME: send key stream */
    ret =
        gst_mpp_buffer_pool_process (GST_MPP_BUFFER_POOL (self->mpp_input->
            pool), &codec_data);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
    gst_buffer_unref (codec_data);

    gst_mpp_object_timeout (self->mpp_output, -1L);

    if (gst_mpp_object_acquire_output_format (self->mpp_output))
      goto not_negotiated;

    if (!gst_mpp_video_dec_src_negotiate (decoder)) {
      if (GST_PAD_IS_FLUSHING (decoder->srcpad))
        goto flushing;
      else
        goto not_negotiated;
    }
    /* activate the pool: the buffers are allocated */
    if (gst_buffer_pool_set_active (self->mpp_output->pool, TRUE) == FALSE)
      goto activate_failed;

    gst_mpp_object_info_change (self->mpp_output);
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
    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    ret =
        gst_mpp_buffer_pool_process (GST_MPP_BUFFER_POOL (self->
            mpp_input->pool), &frame->input_buffer);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);

    if (ret == GST_FLOW_FLUSHING) {
      if (gst_pad_get_task_state (GST_VIDEO_DECODER_SRC_PAD (self)) !=
          GST_TASK_STARTED)
        ret = self->output_flow;
      goto drop;
    } else if (ret != GST_FLOW_OK) {
      goto process_failed;
    }
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
activate_failed:
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
    ret = GST_FLOW_NOT_NEGOTIATED;
    goto drop;
  }
process_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        ("Failed to process frame."),
        ("Maybe be due to not enough memory or failing driver"));
    ret = GST_FLOW_ERROR;
    goto drop;
  }
drop:
  {
    GST_ERROR_OBJECT (self, "can't process this frame");
    gst_video_decoder_drop_frame (decoder, frame);
    return ret;
  }
}

static gboolean
gst_mpp_video_dec_decide_allocation (GstVideoDecoder * decoder,
    GstQuery * query)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  gboolean ret = FALSE;

  if (gst_mpp_object_decide_allocation (self->mpp_output, query))
    ret = GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation (decoder,
        query);

  return ret;
}

static gboolean
gst_mpp_video_dec_sink_event (GstVideoDecoder * decoder, GstEvent * event)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  gboolean ret;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (self, "flush start");
      gst_mpp_object_unlock (self->mpp_input);
      gst_mpp_object_unlock (self->mpp_output);
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

static gboolean
gst_mpp_video_dec_negotiate (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  if (self->mpp_output->pool &&
      gst_buffer_pool_is_active (GST_BUFFER_POOL (self->mpp_output->pool)))
    return TRUE;

  return GST_VIDEO_DECODER_CLASS (parent_class)->negotiate (decoder);
}

static GstStateChangeReturn
gst_mpp_video_dec_change_state (GstElement * element, GstStateChange transition)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (element);
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    g_atomic_int_set (&self->active, FALSE);
    /* gst_mpp_object_sendeos (self->mpp_input); */
    gst_mpp_object_unlock (self->mpp_input);
    gst_mpp_object_unlock (self->mpp_output);
    gst_pad_stop_task (decoder->srcpad);
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_mpp_video_dec_class_init (GstMppVideoDecClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (mpp_video_dec_debug, "mppvideodec", 0,
      "mpp video decoder");

  gst_element_class_set_static_metadata (element_class,
      "Rockchip's MPP video decoder", "Decoder/Video",
      "Multicodec (MPEG-2/4 / AVC / VP8 / HEVC) hardware decoder",
      "Randy Li <randy.li@rock-chips.com>");

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_mpp_video_dec_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_mpp_video_dec_get_property);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mpp_video_dec_src_template));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mpp_video_dec_sink_template));

  video_decoder_class->open = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_open);
  video_decoder_class->close = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_close);
  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_stop);
  video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_finish);
  video_decoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_mpp_video_dec_set_format);
  video_decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_mpp_video_dec_decide_allocation);
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_mpp_video_dec_handle_frame);
  video_decoder_class->flush = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_flush);
  video_decoder_class->negotiate =
      GST_DEBUG_FUNCPTR (gst_mpp_video_dec_negotiate);
  video_decoder_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_mpp_video_dec_sink_event);

  element_class->change_state = GST_DEBUG_FUNCPTR
      (gst_mpp_video_dec_change_state);

  gst_mpp_object_install_properties_helper (gobject_class);
}

static void
gst_mpp_video_dec_init (GstMppVideoDec * self)
{
  GstVideoDecoder *decoder = (GstVideoDecoder *) self;

  gst_video_decoder_set_packetized (decoder, TRUE);

  self->input_state = NULL;
  self->mpp_input = gst_mpp_object_new (GST_ELEMENT (self), FALSE);
  self->mpp_output = gst_mpp_object_new (GST_ELEMENT (self), FALSE);
}
