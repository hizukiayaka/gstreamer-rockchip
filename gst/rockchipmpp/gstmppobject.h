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

#ifndef  _GST_MPP_OBJECT_H_
#define  _GST_MPP_OBJECT_H_

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideopool.h>

#include <rockchip/rk_mpi.h>

#define GST_MPP_MIN_BUFFERS     16

G_BEGIN_DECLS
typedef struct _GstMppObject GstMppObject;
typedef enum _GstMppReturn GstMppReturn;

enum _GstMppReturn
{
  GST_MPP_OK = 0,
  GST_MPP_ERROR = -1,
  GST_MPP_BUSY = -2
};

typedef enum
{
  GST_MPP_DEC_INPUT = 0,
  GST_MPP_DEC_OUTPUT = 1,
  GST_MPP_ENC_INPUT = 2,
  GST_MPP_ENC_OUTPUT = 3,
} GstMppNodeMode;

#define GST_TYPE_MPP_IO_MODE (gst_mpp_io_mode_get_type ())

typedef enum
{
  GST_MPP_IO_AUTO = 0,
  /* Mpp internal buffer in pool */
  GST_MPP_IO_ION = 1,
  GST_MPP_IO_DRMBUF = 2,
  /* External buffer mode */
  GST_MPP_IO_DMABUF_IMPORT = 3,
  /* API, internal copy */
  GST_MPP_IO_RW = 4,
  /* Reversed */
  GST_MPP_IO_USERPTR = 5,
} GstMppIOMode;

GType gst_mpp_io_mode_get_type (void);

#define MPP_STD_OBJECT_PROPS \
	PROP_IO_MODE

#define GST_MPP_OBJECT(obj) (GstMppObject *)(obj)

#define GST_MPP_WIDTH(o)           (GST_VIDEO_INFO_WIDTH (&(o)->info))
#define GST_MPP_HEIGHT(o)          (GST_VIDEO_INFO_HEIGHT (&(o)->info))
#define GST_MPP_FPS_N(o)           (GST_VIDEO_INFO_FPS_N (&(o)->info))
#define GST_MPP_FPS_D(o)           (GST_VIDEO_INFO_FPS_D (&(o)->info))
#define GST_MPP_SIZE(o)            (GST_VIDEO_INFO_SIZE (&(o)->info))
#define GST_MPP_INTERLACE_MODE(o)  (GST_VIDEO_INFO_INTERLACE_MODE (&(o)->info))
#define GST_MPP_PIXELFORMAT(o)     (GST_VIDEO_INFO_FORMAT (&(o)->info))
#define GST_MPP_ALIGN(o)           (&(o)->align_info)

#define GST_MPP_IS_ACTIVE(o)    ((o)->active)
#define GST_MPP_SET_ACTIVE(o)   ((o)->active = TRUE)
#define GST_MPP_SET_INACTIVE(o) ((o)->active = FALSE)

struct _GstMppObject
{
  GstElement *element;

  /* Rockchip Mpp definitions */
  MppCtx mpp_ctx;
  MppApi *mpi;
  GstMppNodeMode type;

  /* the currently format */
  GstVideoInfo info;
  GstVideoInfo align_info;
  gboolean need_video_meta;

  /* State */
  gboolean active;

  /* Pool of the object */
  GstMppIOMode req_mode;
  GstMppIOMode mode;
  GstBufferPool *pool;
};

GType gst_mpp_object_get_type (void);

void gst_mpp_object_unlock (GstMppObject * self);
void gst_mpp_object_unlock_stop (GstMppObject * self);
gboolean gst_mpp_object_flush (GstMppObject * self);

GstMppObject *gst_mpp_object_new (GstElement * element, gboolean is_encoder);
gboolean gst_mpp_object_open (GstMppObject * self);
GstMppObject *gst_mpp_object_open_shared (GstMppObject *self, GstMppObject *other);
gboolean gst_mpp_object_set_fmt (GstMppObject * self, GstCaps * caps);
gboolean gst_mpp_object_destroy (GstMppObject * self);

gboolean gst_mpp_object_sendeos (GstMppObject * self);
GstMppReturn gst_mpp_object_acquire_output_format (GstMppObject * self);
void gst_mpp_object_info_change (GstMppObject * self);
gboolean gst_mpp_object_timeout (GstMppObject * self, gint64 timeout);
gboolean gst_mpp_object_setup_pool (GstMppObject * self, GstCaps * caps);
void gst_mpp_object_close_pool (GstMppObject * self);

GstMppReturn gst_mpp_object_send_stream (GstMppObject * self, GstBuffer * data);

void gst_mpp_object_install_properties_helper (GObjectClass * gobject_class);
gboolean gst_mpp_object_get_property_helper (GstMppObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);

gboolean gst_mpp_object_set_property_helper (GstMppObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);

void gst_mpp_object_config_pool (GstMppObject * self, gpointer pool);

GstFlowReturn
gst_mpp_object_dec_frame (GstMppObject * self, MppFrame * out_frame);

gboolean
gst_mpp_object_decide_allocation (GstMppObject * obj, GstQuery * query);

G_END_DECLS
#endif /* _GST_MPP_OBJECT_H_ */
