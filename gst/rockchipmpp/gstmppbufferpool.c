/*
 * Copyright 2017 - 2018 SUMOMO Computer Association
 *     Author: Randy Li <ayaka@soulik.info>
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
#include <config.h>
#endif

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <gst/gst-i18n-plugin.h>
#include "gstmppbufferpool.h"

GST_DEBUG_CATEGORY_STATIC (mppbufferpool_debug);
GST_DEBUG_CATEGORY_STATIC (CAT_PERFORMANCE);
#define GST_CAT_DEFAULT mppbufferpool_debug

#define GST_MPP_IMPORT_QUARK gst_mpp_buffer_pool_import_quark ()
/*
 * GstMppBufferPool:
 */
#define parent_class gst_mpp_buffer_pool_parent_class
G_DEFINE_TYPE (GstMppBufferPool, gst_mpp_buffer_pool, GST_TYPE_BUFFER_POOL);

static GQuark
gst_mpp_buffer_pool_import_quark (void)
{
  static GQuark quark = 0;

  if (quark == 0)
    quark = g_quark_from_string ("GstMppBufferPoolUsePtrData");

  return quark;
}

static gboolean
gst_mpp_is_buffer_valid (GstBuffer * buffer, GstMppMemory ** out_mem)
{
  GstMemory *mem = gst_buffer_peek_memory (buffer, 0);
  gboolean valid = FALSE;

  if (gst_is_dmabuf_memory (mem))
    mem = gst_mini_object_get_qdata (GST_MINI_OBJECT (mem),
        GST_MPP_MEMORY_QUARK);
  else
    goto done;

  if (mem && gst_is_mpp_memory (mem)) {
    *out_mem = (GstMppMemory *) mem;
    valid = TRUE;
  }

done:
  return valid;
}

static GstMemory *
gst_mpp_buffer_pool_import_dmabuf (GstMppBufferPool * pool, GstBuffer * src)
{
  GstMppMemory *mem = NULL;
  GstMemory *dma_mems[GST_VIDEO_MAX_PLANES] = { 0 };
  GstMemory *dma_mem = NULL;
  guint n_mem = gst_buffer_n_memory (src);
  gint i;

  GST_LOG_OBJECT (pool, "importing dmabuf");

  if (n_mem > 1) {
    GST_ERROR_OBJECT (pool, "don't support multiple memory blocks now");
    return NULL;
  }

  for (i = 0; i < n_mem; i++)
    dma_mems[i] = gst_buffer_peek_memory (src, i);

  mem = gst_mpp_allocator_import_dmabuf (pool->vallocator, dma_mems, n_mem);
  if (!mem) {
    GST_ERROR_OBJECT (pool, "can't import a dmabuf");
    return NULL;
  }

  dma_mem = gst_mpp_allocator_export_dmabuf (pool->allocator, mem);
  if (!dma_mem)
    GST_ERROR_OBJECT (pool, "can't create a dmabuf");

  return dma_mem;
}

static gboolean
gst_mpp_buffer_pool_start (GstBufferPool * bpool)
{
  GstMppBufferPool *pool = GST_MPP_BUFFER_POOL (bpool);
  GstBufferPoolClass *pclass = GST_BUFFER_POOL_CLASS (parent_class);
  GstMppObject *obj = pool->obj;
  GstStructure *config;
  GstCaps *caps;
  guint size, min_buffers, max_buffers, count;

  GST_DEBUG_OBJECT (pool, "start pool %p", pool);

  config = gst_buffer_pool_get_config (bpool);
  if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers,
          &max_buffers))
    goto wrong_config;

  switch (obj->mode) {
    case GST_MPP_IO_ION:
    case GST_MPP_IO_DRMBUF:
    {
      GST_DEBUG_OBJECT (pool, "requesting %d DMABUF buffers", min_buffers);

      count = gst_mpp_allocator_start (pool->vallocator, min_buffers,
          obj->mode);
      if (count < min_buffers)
        goto no_buffers;

      min_buffers = count;
      break;
    }
    case GST_MPP_IO_DMABUF_IMPORT:
    {
      GST_DEBUG_OBJECT (pool, "will import %d DMABUF buffers", min_buffers);

      count = gst_mpp_allocator_start (pool->vallocator, min_buffers,
          obj->mode);
      if (count < min_buffers)
        goto no_buffers;

      min_buffers = count;
      break;
    }
    case GST_MPP_IO_RW:
      return TRUE;
    default:
      g_assert_not_reached ();
      break;
  }

  pool->size = size;
  pool->num_queued = 0;

  if (max_buffers != 0 && max_buffers < min_buffers)
    max_buffers = min_buffers;

  gst_buffer_pool_config_set_params (config, caps, size, min_buffers,
      max_buffers);
  pclass->set_config (bpool, config);
  gst_structure_free (config);

  if (pool->other_pool)
    if (!gst_buffer_pool_set_active (pool->other_pool, TRUE))
      goto other_pool_failed;

  /* allocate the buffers */
  if (!pclass->start (bpool))
    goto start_failed;

/* TODO: Move to allcator */
#if 1
  gst_mpp_object_config_pool (pool->obj,
      (gpointer) pool->vallocator->mpp_mem_pool);
#endif

  return TRUE;

  /* ERRORS */
wrong_config:
  {
    GST_ERROR_OBJECT (pool, "invalid config %" GST_PTR_FORMAT, config);
    return FALSE;
  }
no_buffers:
  {
    GST_ERROR_OBJECT (pool,
        "we received %d buffer, we want at least %d", count, min_buffers);
    gst_structure_free (config);
    return FALSE;
  }
start_failed:
  {
    GST_ERROR_OBJECT (pool, "failed to start pool %p", pool);
    return FALSE;
  }
other_pool_failed:
  {
    GST_ERROR_OBJECT (pool, "failed to active the other pool %"
        GST_PTR_FORMAT, pool->other_pool);
    return FALSE;
  }
}

static gboolean
gst_mpp_buffer_pool_stop (GstBufferPool * bpool)
{
  GstMppBufferPool *pool = GST_MPP_BUFFER_POOL (bpool);
  GstBufferPoolClass *pclass = GST_BUFFER_POOL_CLASS (parent_class);
  gboolean ret = FALSE;
  gint i;

  GST_DEBUG_OBJECT (pool, "stop pool %p", pool);

  if (pool->other_pool) {
    gst_buffer_pool_set_active (pool->other_pool, FALSE);
    gst_object_unref (pool->other_pool);
    pool->other_pool = NULL;
  }

/* TODO: Move to allcator */
#if 1
  gst_mpp_object_config_pool (pool->obj, (gpointer) NULL);
#endif
  for (i = 0; i < VIDEO_MAX_FRAME; i++) {
    if (pool->buffers[i]) {
      GstBuffer *buffer = pool->buffers[i];
      GstBufferPool *bpool = GST_BUFFER_POOL (pool);

      pool->buffers[i] = NULL;
      pclass->release_buffer (bpool, buffer);
      g_atomic_int_add (&pool->num_queued, -1);
    }
  }

  ret = GST_BUFFER_POOL_CLASS (parent_class)->stop (bpool);

  if (ret && pool->vallocator) {
    gint vret;
    vret = gst_mpp_allocator_stop (pool->vallocator);

    ret = (vret == 0);
  }

  return ret;
}

static GstFlowReturn
gst_mpp_buffer_pool_poll (GstMppBufferPool * pool)
{
  GST_OBJECT_LOCK (pool);
  while (pool->empty)
    g_cond_wait (&pool->empty_cond, GST_OBJECT_GET_LOCK (pool));
  GST_OBJECT_UNLOCK (pool);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_mpp_buffer_pool_alloc_buffer (GstBufferPool * bpool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GstMppBufferPool *pool = GST_MPP_BUFFER_POOL (bpool);
  GstMppObject *obj = pool->obj;
  GstVideoInfo *info = &obj->info;
  GstMemory *mem = NULL;
  GstBuffer *newbuf = NULL;
  GstFlowReturn ret;
  GstBuffer *src = NULL;

  switch (obj->mode) {
    case GST_MPP_IO_RW:
      newbuf =
          gst_buffer_new_allocate (pool->allocator, pool->size, &pool->params);
      break;
    case GST_MPP_IO_ION:
    case GST_MPP_IO_DRMBUF:
      mem = gst_mpp_allocator_alloc_dmabuf (pool->vallocator, pool->allocator);
      break;
    case GST_MPP_IO_DMABUF_IMPORT:
    {
      if (pool->other_pool == NULL) {
        GST_ERROR_OBJECT (pool, "can't prepare buffer, source buffer missing");
        return GST_FLOW_ERROR;
      }

      ret = gst_buffer_pool_acquire_buffer (pool->other_pool, &src, NULL);
      if (ret != GST_FLOW_OK) {
        GST_ERROR_OBJECT (pool,
            "failed to acquire buffer from downstream pool");
        return ret;
      }
      mem = gst_mpp_buffer_pool_import_dmabuf (pool, src);
    }
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  if (mem != NULL) {
    newbuf = gst_buffer_new ();
    gst_buffer_append_memory (newbuf, mem);
  } else if (newbuf == NULL) {
    goto allocation_failed;
  }

  if (src) {
    gst_mini_object_set_qdata (GST_MINI_OBJECT (newbuf), GST_MPP_IMPORT_QUARK,
        gst_buffer_ref (src), (GDestroyNotify) gst_buffer_unref);

    gst_buffer_copy_into (newbuf, src,
        GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS, 0, -1);
    gst_buffer_unref (src);
  }

  if (pool->add_videometa)
    gst_buffer_add_video_meta_full (newbuf, GST_VIDEO_FRAME_FLAG_NONE,
        GST_VIDEO_INFO_FORMAT (info), GST_VIDEO_INFO_WIDTH (info),
        GST_VIDEO_INFO_HEIGHT (info), GST_VIDEO_INFO_N_PLANES (info),
        info->offset, info->stride);

  *buffer = newbuf;

  return GST_FLOW_OK;

  /* ERRORS */
allocation_failed:
  {
    GST_ERROR_OBJECT (pool, "failed to allocate buffer");
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
gst_mpp_buffer_pool_qbuf (GstMppBufferPool * pool, GstBuffer * buffer)
{
  GstMppMemory *mem = NULL;
  gint index;

  if (!gst_mpp_is_buffer_valid (buffer, &mem)) {
    GST_ERROR_OBJECT (pool, "can't release an invalid buffer");
    return GST_FLOW_ERROR;
  }

  index = mpp_buffer_get_index (mem->mpp_buf);

  if (pool->buffers[index] != NULL)
    goto already_queued;

  GST_OBJECT_LOCK (pool);
  g_atomic_int_add (&pool->num_queued, 1);
  pool->buffers[index] = buffer;

  if (!gst_mpp_allocator_qbuf (pool->vallocator, mem))
    goto queue_failed;

  pool->empty = FALSE;
  g_cond_signal (&pool->empty_cond);
  GST_OBJECT_UNLOCK (pool);

  return GST_FLOW_OK;
  /* ERRORS */
already_queued:
  {
    GST_ERROR_OBJECT (pool, "the buffer %i was already queued", index);
    return GST_FLOW_ERROR;
  }
queue_failed:
  {
    GST_ERROR_OBJECT (pool, "could not queue a buffer %i", index);
    /* Mark broken buffer to the allocator */
    g_atomic_int_add (&pool->num_queued, -1);
    g_atomic_int_add (&pool->num_queued, -1);
    pool->buffers[index] = NULL;
    GST_OBJECT_UNLOCK (pool);
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
gst_mpp_buffer_pool_dqbuf (GstMppBufferPool * pool, GstBuffer ** buffer)
{
  GstFlowReturn res;
  GstBuffer *outbuf = NULL;
  GstMppMemory *mem = NULL;
  gint field;
  gint64 pts, dts;

  if ((res = gst_mpp_buffer_pool_poll (pool)) != GST_FLOW_OK)
    goto poll_failed;

  GST_LOG_OBJECT (pool, "dequeueing a buffer");

  res = gst_mpp_allocator_dqbuf (pool->vallocator, &mem);
  switch (res) {
    case GST_FLOW_OK:
      break;
    case GST_FLOW_EOS:
      goto eos;
      break;
    default:
      goto dqbuf_failed;
      break;
  }

  outbuf = pool->buffers[mem->index];
  if (outbuf == NULL)
    goto no_buffer;

  pool->buffers[mem->index] = NULL;
  if (g_atomic_int_dec_and_test (&pool->num_queued)) {
    GST_OBJECT_LOCK (pool);
    pool->empty = TRUE;
    g_cond_signal (&pool->empty_cond);
    GST_OBJECT_UNLOCK (pool);
  }

  field = mpp_frame_get_mode (mem->data) & MPP_FRAME_FLAG_FIELD_ORDER_MASK;
  /* TODO: Ignore timestamp and field for input node */
  switch (field & MPP_FRAME_FLAG_DEINTERLACED) {
    case MPP_FRAME_FLAG_TOP_FIRST:
      GST_BUFFER_FLAG_SET (outbuf, GST_VIDEO_BUFFER_FLAG_INTERLACED);
      GST_BUFFER_FLAG_SET (outbuf, GST_VIDEO_BUFFER_FLAG_TFF);
      break;
    case MPP_FRAME_FLAG_BOT_FIRST:
      GST_BUFFER_FLAG_SET (outbuf, GST_VIDEO_BUFFER_FLAG_INTERLACED);
      GST_BUFFER_FLAG_UNSET (outbuf, GST_VIDEO_BUFFER_FLAG_TFF);
      break;
    default:
      GST_BUFFER_FLAG_UNSET (outbuf, GST_VIDEO_BUFFER_FLAG_INTERLACED);
      GST_BUFFER_FLAG_UNSET (outbuf, GST_VIDEO_BUFFER_FLAG_TFF);
      break;
  }

  /* TODO: set GstBufferFlags for encoder encoded data */

  if (mpp_frame_get_errinfo (mem->data))
    GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_CORRUPTED);
  if (mpp_frame_get_discard (mem->data)) {
    GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_CORRUPTED);
    GST_BUFFER_FLAG_SET (outbuf, GST_BUFFER_FLAG_DECODE_ONLY);
  }

  pts = mpp_frame_get_pts (mem->data);
  GST_BUFFER_PTS (outbuf) = pts;
  dts = mpp_frame_get_dts (mem->data);
  GST_BUFFER_DTS (outbuf) = dts;

  /* FIXME: only work for decoder */
  mpp_frame_deinit (&mem->data);
  mem->data = NULL;

  *buffer = outbuf;

  return res;
  /* ERRORS */
poll_failed:
  {
    GST_DEBUG_OBJECT (pool, "poll error %s", gst_flow_get_name (res));
    return res;
  }
eos:
  {
    return GST_FLOW_EOS;
  }
dqbuf_failed:
  {
    return res;
  }
no_buffer:
  {
    GST_ERROR_OBJECT (pool,
        "mpp output to an occupy buffer, which is not in the pool");
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
gst_mpp_buffer_pool_acquire_buffer (GstBufferPool * bpool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GstMppBufferPool *pool = GST_MPP_BUFFER_POOL (bpool);
  GstMppObject *obj = pool->obj;
  GstFlowReturn ret;

  switch (obj->type) {
    case GST_MPP_DEC_OUTPUT:
      ret = gst_mpp_buffer_pool_dqbuf (pool, buffer);
      break;
    default:
      ret = GST_FLOW_ERROR;
      g_assert_not_reached ();
      break;
  }

  return ret;
}

static void
gst_mpp_buffer_pool_release_buffer (GstBufferPool * bpool, GstBuffer * buffer)
{
  GstMppBufferPool *pool = GST_MPP_BUFFER_POOL (bpool);
  GstBufferPoolClass *pclass = GST_BUFFER_POOL_CLASS (parent_class);
  GstMppObject *obj = pool->obj;

  GST_DEBUG_OBJECT (pool, "release buffer %p", buffer);

  switch (obj->type) {
    case GST_MPP_DEC_OUTPUT:{
      GstMppMemory *mem = NULL;

      if (gst_mpp_is_buffer_valid (buffer, &mem)) {
#if 0
        gst_mpp_allocator_reset_memory (pool->vallocator, mem);
        if (pool->other_pool)
          gst_mpp_buffer_pool_prepare_buffer (pool, buffer, NULL);
#endif
        if (gst_mpp_buffer_pool_qbuf (pool, buffer) != GST_FLOW_OK)
          pclass->release_buffer (bpool, buffer);
      } else {
        GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_TAG_MEMORY);
        pclass->release_buffer (bpool, buffer);
      }
    }
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  GST_DEBUG_OBJECT (pool,
      "released buffer %p, queued %d", buffer,
      g_atomic_int_get (&pool->num_queued));

  return;
}

static void
gst_mpp_buffer_pool_flush_start (GstBufferPool * bpool)
{
  GstMppBufferPool *pool = GST_MPP_BUFFER_POOL (bpool);

  GST_DEBUG_OBJECT (pool, "start flushing");

  GST_OBJECT_LOCK (pool);
  pool->empty = FALSE;
  g_cond_broadcast (&pool->empty_cond);
  GST_OBJECT_UNLOCK (pool);

  if (pool->other_pool)
    gst_buffer_pool_set_flushing (pool->other_pool, TRUE);
}

static void
gst_mpp_buffer_pool_flush_stop (GstBufferPool * bpool)
{
  GstMppBufferPool *pool = GST_MPP_BUFFER_POOL (bpool);

  GST_DEBUG_OBJECT (pool, "stop flushing");

  if (pool->other_pool)
    gst_buffer_pool_set_flushing (pool->other_pool, FALSE);
}

static gboolean
gst_mpp_buffer_pool_set_config (GstBufferPool * bpool, GstStructure * config)
{
  GstMppBufferPool *pool = GST_MPP_BUFFER_POOL (bpool);
  GstMppObject *obj = pool->obj;
  GstCaps *caps;
  guint size, min_buffers, max_buffers;
  GstAllocator *allocator;
  GstAllocationParams params;
  gboolean updated = FALSE;
  gboolean ret;

  pool->add_videometa =
      gst_buffer_pool_config_has_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_META);

  if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers,
          &max_buffers))
    goto wrong_config;

  if (!gst_buffer_pool_config_get_allocator (config, &allocator, &params))
    goto wrong_config;

  GST_DEBUG_OBJECT (pool, "config %" GST_PTR_FORMAT, config);

  if (pool->allocator)
    gst_object_unref (pool->allocator);

  pool->allocator = NULL;

  switch (obj->mode) {
    case GST_MPP_IO_ION:
    case GST_MPP_IO_DRMBUF:
    case GST_MPP_IO_DMABUF_IMPORT:
      pool->allocator = gst_dmabuf_allocator_new ();
      break;
    case GST_MPP_IO_RW:
      if (allocator)
        pool->allocator = g_object_ref (allocator);
      pool->params = params;
      goto done;
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  if (max_buffers > VIDEO_MAX_FRAME || max_buffers == 0) {
    updated = TRUE;
    max_buffers = VIDEO_MAX_FRAME;
    GST_INFO_OBJECT (pool, "reducing maximum buffers to %u", max_buffers);
  }

  if (min_buffers > max_buffers) {
    updated = TRUE;
    min_buffers = max_buffers;
    GST_INFO_OBJECT (pool, "reducing minimum buffers to %u", min_buffers);
  }

  if (!pool->add_videometa && obj->need_video_meta) {
    GST_INFO_OBJECT (pool, "adding needed video meta");
    updated = TRUE;
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
  }

  gst_buffer_pool_config_set_params (config, caps, size, min_buffers,
      max_buffers);

  gst_video_info_from_caps (&pool->caps_info, caps);

done:
  ret = GST_BUFFER_POOL_CLASS (parent_class)->set_config (bpool, config);

  return !updated && ret;

  /* ERROR */
wrong_config:
  {
    GST_ERROR_OBJECT (pool, "invalid config %" GST_PTR_FORMAT, config);
    return FALSE;
  }
}

static void
gst_mpp_buffer_pool_dispose (GObject * object)
{
  GstMppBufferPool *pool = GST_MPP_BUFFER_POOL (object);

  if (pool->vallocator)
    gst_object_unref (pool->vallocator);
  pool->vallocator = NULL;

  if (pool->allocator)
    gst_object_unref (pool->allocator);
  pool->allocator = NULL;

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_mpp_buffer_pool_finalize (GObject * object)
{
  GstMppBufferPool *pool = GST_MPP_BUFFER_POOL (object);

  g_cond_clear (&pool->empty_cond);
  /* FIXME: unbinding the external buffer of the rockchip mpp */
  gst_object_unref (pool->obj->element);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_mpp_buffer_pool_init (GstMppBufferPool * pool)
{
  pool->obj = NULL;
  pool->num_queued = 0;
  pool->other_pool = NULL;
  g_cond_init (&pool->empty_cond);
  pool->empty = TRUE;
}

static void
gst_mpp_buffer_pool_class_init (GstMppBufferPoolClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstBufferPoolClass *bufferpool_class = GST_BUFFER_POOL_CLASS (klass);

  object_class->dispose = gst_mpp_buffer_pool_dispose;
  object_class->finalize = gst_mpp_buffer_pool_finalize;

  bufferpool_class->start = gst_mpp_buffer_pool_start;
  bufferpool_class->stop = gst_mpp_buffer_pool_stop;
  bufferpool_class->set_config = gst_mpp_buffer_pool_set_config;
  bufferpool_class->alloc_buffer = gst_mpp_buffer_pool_alloc_buffer;
  bufferpool_class->acquire_buffer = gst_mpp_buffer_pool_acquire_buffer;
  bufferpool_class->release_buffer = gst_mpp_buffer_pool_release_buffer;
  bufferpool_class->flush_start = gst_mpp_buffer_pool_flush_start;
  bufferpool_class->flush_stop = gst_mpp_buffer_pool_flush_stop;

  GST_DEBUG_CATEGORY_INIT (mppbufferpool_debug, "mppbufferpool", 0,
      "mpp buffer pool");
  GST_DEBUG_CATEGORY_GET (CAT_PERFORMANCE, "GST_PERFORMANCE");
}

/**
 * gst_mpp_buffer_pool_new:
 * @dec: the mpp decoder owning the pool
 * @max: maximum buffers in the pool
 * @size: size of the buffer
 *
 * Construct a new buffer pool.
 *
 * Returns: the new pool, use gst_object_unref() to free resources
 */
GstBufferPool *
gst_mpp_buffer_pool_new (GstMppObject * obj, GstCaps * caps)
{
  GstMppBufferPool *pool;
  GstStructure *config;
  gchar *name, *parent_name;

  /* setting a significant unique name */
  parent_name = gst_object_get_name (GST_OBJECT (obj->element));
  name = g_strconcat (parent_name, ":", "pool:",
      /* FIXME correct the node name */
      obj->type == GST_MPP_DEC_INPUT ? "sink" : "src", NULL);
  g_free (parent_name);

  pool =
      (GstMppBufferPool *) g_object_new (GST_TYPE_MPP_BUFFER_POOL,
      "name", name, NULL);
  g_object_ref_sink (pool);
  g_free (name);

  /* take a reference on mpp object to be sure that it will be released
   * after the pool */
  pool->obj = obj;
  gst_object_ref (obj->element);
  /* The default MPP DMA Allocator */
  pool->vallocator = gst_mpp_allocator_new (GST_OBJECT (pool), obj);
  if (!pool->vallocator)
    goto allocator_failed;

  config = gst_buffer_pool_get_config (GST_BUFFER_POOL_CAST (pool));
  gst_buffer_pool_config_set_params (config, caps, 0, 0, 0);
  /* This will simply set a default config, but will not configure the pool
   * because min and max are not valid */
  gst_buffer_pool_set_config (GST_BUFFER_POOL_CAST (pool), config);

  return GST_BUFFER_POOL (pool);
  /* ERROR */
allocator_failed:
  {
    GST_ERROR_OBJECT (pool, "Failed to create mpp allocator");
    gst_object_unref (pool);
    return NULL;
  }
}

void
gst_mpp_buffer_pool_set_other_pool (GstMppBufferPool * pool,
    GstBufferPool * other_pool)
{
  g_return_if_fail (!gst_buffer_pool_is_active (GST_BUFFER_POOL (pool)));

  if (pool->other_pool)
    gst_object_unref (pool->other_pool);
  pool->other_pool = gst_object_ref (other_pool);
}

GstFlowReturn
gst_mpp_buffer_pool_process (GstMppBufferPool * pool, GstBuffer ** buf)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstBufferPool *bpool = GST_BUFFER_POOL_CAST (pool);
  GstMppObject *obj = pool->obj;

  GST_DEBUG_OBJECT (pool, "process buffer %p", buf);

  if (GST_BUFFER_POOL_IS_FLUSHING (pool))
    return GST_FLOW_FLUSHING;

  switch (obj->type) {
    case GST_MPP_DEC_INPUT:
      switch (obj->mode) {
        case GST_MPP_IO_RW:
        {
          GstMppReturn mret;
          /* TODO: support more error type */
          do {
            mret = gst_mpp_object_send_stream (obj, *buf);
          } while (mret == GST_MPP_BUSY);
        }
          break;
        default:
          g_assert_not_reached ();
          break;
      }
      break;
    case GST_MPP_DEC_OUTPUT:
      switch (obj->mode) {
        case GST_MPP_IO_ION:
        case GST_MPP_IO_DRMBUF:{
          /* The decoder is using the same pool as the output object */
          if ((*buf)->pool == bpool) {
            guint num_queued;
            guint size = gst_buffer_get_size (*buf);

            if (G_UNLIKELY (GST_BUFFER_FLAG_IS_SET (*buf,
                        GST_BUFFER_FLAG_CORRUPTED)))
              goto buffer_corrupted;

            if (size == 0)
              goto eos;

            num_queued = g_atomic_int_get (&pool->num_queued);
            GST_TRACE_OBJECT (pool, "Only %i buffer left in the capture queue.",
                num_queued);
#if 0
            /* If we have no more buffer, and can allocate it time to do so */
            if (num_queued == 0) {
            }
#endif
            ret = GST_FLOW_OK;
            goto done;
          }
        }
          break;
        case GST_MPP_IO_DMABUF_IMPORT:
        {
          /* The decoder is using the same pool as the output object */
          if ((*buf)->pool == bpool) {
            guint num_queued;
            guint size = gst_buffer_get_size (*buf);

            if (G_UNLIKELY (GST_BUFFER_FLAG_IS_SET (*buf,
                        GST_BUFFER_FLAG_CORRUPTED)))
              goto buffer_corrupted;

            if (size == 0)
              goto eos;

            num_queued = g_atomic_int_get (&pool->num_queued);
            GST_TRACE_OBJECT (pool, "Only %i buffer left in the capture queue.",
                num_queued);
#if 0
            /* If we have no more buffer, and can allocate it time to do so */
            if (num_queued == 0) {
            }
#endif
            ret = GST_FLOW_OK;
            goto done;

          }
        }
          break;
        default:
          g_assert_not_reached ();
          break;
      }
      break;
    default:
      g_assert_not_reached ();
      break;
  }
done:
  return ret;

  /* ERRORS */
buffer_corrupted:
  {
    GST_WARNING_OBJECT (pool, "Dropping corrupted buffer without payload");
    gst_buffer_unref (*buf);
    *buf = NULL;
    return GST_MPP_FLOW_CORRUPTED_BUFFER;
  }
eos:
  {
    GST_DEBUG_OBJECT (pool, "end of stream reached");
    gst_buffer_unref (*buf);
    *buf = NULL;
    return GST_MPP_FLOW_LAST_BUFFER;
  }
}
