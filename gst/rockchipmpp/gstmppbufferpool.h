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

#ifndef __GST_MPP_BUFFER_POOL_H__
#define __GST_MPP_BUFFER_POOL_H__

#include <gst/gst.h>
#include <gst/allocators/gstdmabuf.h>

#include "gstmppallocator.h"

G_BEGIN_DECLS typedef struct _GstMppBufferPool GstMppBufferPool;
typedef struct _GstMppBufferPoolClass GstMppBufferPoolClass;

#define GST_TYPE_MPP_BUFFER_POOL      (gst_mpp_buffer_pool_get_type())
#define GST_IS_MPP_BUFFER_POOL(obj)   (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_MPP_BUFFER_POOL))
#define GST_MPP_BUFFER_POOL(obj)      (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_MPP_BUFFER_POOL, GstMppBufferPool))
#define GST_MPP_BUFFER_POOL_CAST(obj) ((GstMppBufferPool *)(obj))

#define GST_MPP_FLOW_LAST_BUFFER GST_FLOW_CUSTOM_SUCCESS
#define GST_MPP_FLOW_CORRUPTED_BUFFER GST_FLOW_CUSTOM_SUCCESS_1

struct _GstMppBufferPool
{
  GstBufferPool parent;
  GstMppObject *obj;

  /* number of buffers queued in the mpp and gstmppbufferpool */
  guint num_queued;

  GstBuffer *buffers[VIDEO_MAX_FRAME];

  gboolean empty;
  GCond empty_cond;

  GstMppAllocator *vallocator;
  GstAllocator *allocator;
  GstAllocationParams params;
  GstBufferPool *other_pool;
  guint size;
  /* Default video information */
  GstVideoInfo caps_info;

  /* set if video meta should be added */
  gboolean add_videometa;

  gboolean flushing;
};

struct _GstMppBufferPoolClass
{
  GstBufferPoolClass parent_class;
};

GType gst_mpp_buffer_pool_get_type (void);

GstBufferPool *gst_mpp_buffer_pool_new (GstMppObject * mppobj, GstCaps * caps);

void gst_mpp_buffer_pool_set_other_pool (GstMppBufferPool * pool,
    GstBufferPool * other_pool);

GstFlowReturn
gst_mpp_buffer_pool_process (GstMppBufferPool * pool, GstBuffer ** buf);

G_END_DECLS
#endif /*__GST_MPP_BUFFER_POOL_H__ */
