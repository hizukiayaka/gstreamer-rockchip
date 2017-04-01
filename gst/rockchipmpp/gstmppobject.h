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

#define MPP_ALIGN(x, a)         (((x)+(a)-1)&~((a)-1))

G_BEGIN_DECLS
#define GST_TYPE_MPP_OBJECT	(gst_mpp_video_dec_get_type())
#define GST_MPP_OBJECT(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_MPP_OBJECT, GstMppVideoDec))
#define GST_MPP_OBJECT_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_MPP_OBJECT, GstMppVideoDecClass))
#define GST_IS_MPP_OBJECT(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_MPP_OBJECT))
#define GST_IS_MPP_OBJECT_CLASS(obj) \
	(G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_MPP_OBJECT))
typedef struct _GstMppObject GstMppObject;
typedef struct _GstMppObjectClass GstMppObjectClass;
typedef enum _GstMppReturn GstMppReturn;

enum _GstMppReturn {
  GST_MPP_OK = 0,
  GST_MPP_ERROR = -1,
  GST_MPP_BUSY = -2
};

#define GST_TYPE_MPP_IO_MODE (gst_mpp_io_mode_get_type ())
GType gst_v4l2_mpp_mode_get_type (void);

typedef enum
{
  GST_MPP_IO_AUTO = 0,
  /* Mpp internal buffer mode */
  GST_MPP_IO_ION = 1,
  GST_MPP_IO_DMABUF = 2,
  /* External buffer mode */
  GST_Mpp_IO_DMABUF_IMPORT = 3,
  GST_MPP_IO_USERPTR = 4,
} GstMppIOMode;

#define GST_MPP_IS_ACTIVE(o)    ((o)->active)
#define GST_MPP_SET_ACTIVE(o)   ((o)->active = TRUE)
#define GST_MPP_SET_INACTIVE(o) ((o)->active = FALSE)

struct _GstMppObject
{
  GstElement *element;

  /* Rockchip Mpp definitions */
  MppCtx mpp_ctx;
  MppApi *mpi;
  /* MPP_CTX_DEC, MPP_CTX_ENC */
  MppCtxType type;

  /* the currently format */
  GstVideoInfo info;
  GstVideoAlignment align;

  /* State */
  gboolean active;

  /* Pool of the object */
  GstMppIOMode req_mode;
  GstMppIOMode mode;
  GstBufferPool *pool;
};

struct _GstMppObjectClass
{
  GstElementClass parent_class;
};

GType gst_mpp_object_get_type (void);

void gst_mpp_object_unlock (GstMppObject * self);
void gst_mpp_object_unlock_stop (GstMppObject * self);
gboolean gst_mpp_object_flush (GstMppObject * self);

GstMppObject *gst_mpp_object_new (GstElement * element, gboolean is_encoder);
gboolean gst_mpp_object_init (GstMppObject *self, GstCaps * caps);
gboolean gst_mpp_object_destroy (GstMppObject * self);

gboolean gst_mpp_object_sendeos (GstMppObject * self);
GstMppReturn gst_mpp_object_acquire_output_format (GstMppObject * self);
void gst_mpp_object_info_change (GstMppObject * self);
void gst_mpp_object_output_block (GstMppObject * self, gint64 timeout);
void gst_mpp_object_setup_pool (GstMppObject * self);
void gst_mpp_object_close_pool (GstMppObject * self);

GstMppReturn
gst_mpp_object_send_stream (GstMppObject * self, GstBuffer * data);

void gst_mpp_object_config_pool (GstMppObject * self, gpointer pool);

GstFlowReturn
gst_mpp_object_fetch_dec_index (GstMppObject * self, gint * out_index);

G_END_DECLS
#endif /* _GST_MPP_OBJECT_H_ */
