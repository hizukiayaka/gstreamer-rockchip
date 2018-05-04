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

#include <gst/gst-i18n-plugin.h>
#include <gst/allocators/gstdmabuf.h>
#include <unistd.h>

#include "gstmppallocator.h"

#define GST_MPP_MEMORY_TYPE "MppMemory"

#define gst_mpp_allocator_parent_class parent_class
G_DEFINE_TYPE (GstMppAllocator, gst_mpp_allocator, GST_TYPE_ALLOCATOR);

GST_DEBUG_CATEGORY_STATIC (mppallocator_debug);
#define GST_CAT_DEFAULT mppallocator_debug

gboolean
gst_is_mpp_memory (GstMemory * mem)
{
  return gst_memory_is_type (mem, GST_MPP_MEMORY_TYPE);
}

GQuark
gst_mpp_memory_quark (void)
{
  static GQuark quark = 0;
  if (quark == 0)
    quark = g_quark_from_string ("GstMppMemory");

  return quark;
}

static inline GstMppMemory *
_mppmem_new (GstMemoryFlags flags, GstAllocator * allocator,
    GstMemory * parent, gsize maxsize, gsize align, gsize offset, gsize size,
    gpointer data, int dmafd, MppBuffer * mpp_buf)
{
  GstMppMemory *mem;

  mem = g_slice_new0 (GstMppMemory);
  gst_memory_init (GST_MEMORY_CAST (mem),
      flags, allocator, parent, maxsize, align, offset, size);

  mem->data = data;
  mem->dmafd = dmafd;
  mem->mpp_buf = mpp_buf;
  mem->index = mpp_buffer_get_index (mpp_buf);

  return mem;
}

static void
_mppmem_free (GstMppMemory * mem)
{
  g_slice_free (GstMppMemory, mem);
}

static gpointer
_mppmem_map (GstMppMemory * mem, gsize maxsize, GstMapFlags flags)
{
  if (!mem->data)
    mem->data = mpp_buffer_get_ptr (mem->mpp_buf);

  return mem->data;
}

static void
_mppmem_unmap (GstMemory * mem)
{
  return;
}

static GstMppMemory *
_mppmem_share (GstMppMemory * mem, gssize offset, gssize size)
{
  GstMppMemory *sub;
  GstMemory *parent;

  /* find the real parent */
  if ((parent = mem->mem.parent) == NULL)
    parent = (GstMemory *) mem;

  if (size == -1)
    size = mem->mem.size - offset;

  /* the shared memory is always readonly */
  sub = _mppmem_new (GST_MINI_OBJECT_FLAGS (parent) |
      GST_MINI_OBJECT_FLAG_LOCK_READONLY, mem->mem.allocator, parent,
      mem->mem.maxsize, mem->mem.align, offset, size, mem->data,
      -1, mem->mpp_buf);

  return sub;
}

static gboolean
_mppmem_is_span (GstMppMemory * mem1, GstMppMemory * mem2, gsize * offset)
{
  if (offset)
    *offset = mem1->mem.offset - mem1->mem.parent->offset;

  /* and memory is contiguous */
  return mem1->mem.offset + mem1->mem.size == mem2->mem.offset;
}

/*
 * GstMppAllocator Implementation
 */

/* Auto clean up methods */
static void
gst_mpp_allocator_dispose (GObject * obj)
{
  GST_LOG_OBJECT (obj, "called");

  G_OBJECT_CLASS (parent_class)->dispose (obj);
}

static void
gst_mpp_allocator_finalize (GObject * obj)
{
  GstMppAllocator *allocator = (GstMppAllocator *) obj;

  GST_LOG_OBJECT (obj, "called");

  if (allocator->mpp_mem_pool) {
    mpp_buffer_group_put (allocator->mpp_mem_pool);
    allocator->mpp_mem_pool = NULL;
  }

  gst_atomic_queue_unref (allocator->free_queue);
  gst_object_unref (allocator->obj->element);

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

/* Manually clean way */
gint
gst_mpp_allocator_stop (GstMppAllocator * allocator)
{
  guint i = 0;
  gint ret = 0;
  GST_DEBUG_OBJECT (allocator, "stop allocator");

  GST_OBJECT_LOCK (allocator);

  if (!g_atomic_int_get (&allocator->active))
    goto done;

  for (i = 0; i < allocator->count; i++) {
    GstMppMemory *mem = allocator->mems[i];
    allocator->mems[i] = NULL;
    if (mem)
      _mppmem_free (mem);
  }

  if (allocator->mpp_mem_pool) {
    mpp_buffer_group_put (allocator->mpp_mem_pool);
    allocator->mpp_mem_pool = NULL;
  }

  allocator->count = 0;

  g_atomic_int_set (&allocator->active, FALSE);
done:
  GST_OBJECT_UNLOCK (allocator);
  return ret;
}

static void
gst_mpp_allocator_free (GstAllocator * gallocator, GstMemory * gmem)
{
  GstMppMemory *mem = (GstMppMemory *) gmem;

  _mppmem_free (mem);
}

GstMppAllocator *
gst_mpp_allocator_new (GstObject * parent, GstMppObject * mppobject)
{
  GstMppAllocator *allocator = NULL;
  gchar *name, *parent_name;

  parent_name = gst_object_get_name (parent);
  name = g_strconcat (parent_name, ":allocator", NULL);
  g_free (parent_name);

  allocator = g_object_new (GST_TYPE_MPP_ALLOCATOR, "name", name, NULL);
  g_free (name);

  allocator->obj = mppobject;
  gst_object_ref (allocator->obj->element);

  return allocator;
}

GstMppMemory *
gst_mpp_allocator_import_dmabuf (GstMppAllocator * allocator,
    GstMemory ** dma_mem, gint n_mem)
{
  guint i;

  if (n_mem > 1)
    goto n_mem_missmatch;

  for (i = 0; i < n_mem; i++) {
    MppBuffer mpp_buf = NULL;
    MppBufferInfo commit = { 0, };
    GstMppMemory *mem = NULL;
    gsize size, offset, maxsize;
    gint dmafd, index;

    if (!gst_is_dmabuf_memory (dma_mem[i]))
      goto not_dmabuf;

    size = gst_memory_get_sizes (dma_mem[i], &offset, &maxsize);
    index = allocator->count;

    if ((dmafd = dup (gst_dmabuf_memory_get_fd (dma_mem[i]))) < 0)
      goto dup_failed;

    GST_LOG_OBJECT (allocator, "imported DMABUF as fd %i buffer %d", dmafd,
        index);

    commit.type = MPP_BUFFER_TYPE_EXT_DMA;
    commit.fd = dmafd;
    commit.size = size;
    commit.index = index;

    /*
     * After this function return success, the inc_buffer_ref_no_lock()
     * in mpp_buffer_create() would increase the internal buffer reference
     * count and remove it from group, remember to put it back to group
     * before future usage
     */
    if (mpp_buffer_import_with_tag (allocator->mpp_mem_pool, &commit,
            &mpp_buf, NULL, __FUNCTION__)) {
      GST_ERROR_OBJECT (allocator, "commit buffer %d failed", index);
      return FALSE;
    }

    mem = _mppmem_new (0, GST_ALLOCATOR (allocator), NULL,
        mpp_buffer_get_size (mpp_buf), 0, 0, mpp_buffer_get_size (mpp_buf),
        NULL, mpp_buffer_get_fd (mpp_buf), mpp_buf);

    allocator->mems[allocator->count] = mem;
    allocator->count++;

    return mem;
  }

n_mem_missmatch:
  {
    GST_ERROR_OBJECT (allocator, "Got %i dmabuf but needed", n_mem);
    return NULL;
  }
not_dmabuf:
  {
    GST_ERROR_OBJECT (allocator, "Memory is not of DMABUF");
    return NULL;
  }
dup_failed:
  {
    GST_ERROR_OBJECT (allocator, "Failed to dup DMABUF descriptor: %s",
        g_strerror (errno));
    return FALSE;
  }
}

GstMemory *
gst_mpp_allocator_export_dmabuf (GstAllocator * dmabuf_allocator,
    GstMppMemory * mem)
{
  GstMemory *dma_mem;

  if (mem->dmafd < 0) {
    GST_ERROR_OBJECT (dmabuf_allocator, "Failed to get dmafd");
    return NULL;
  }

  dma_mem = gst_dmabuf_allocator_alloc (dmabuf_allocator, mem->dmafd,
      mem->mem.size);
  gst_mini_object_set_qdata (GST_MINI_OBJECT (dma_mem),
      GST_MPP_MEMORY_QUARK, mem, (GDestroyNotify) gst_memory_unref);

  return dma_mem;
}

GstMemory *
gst_mpp_allocator_alloc_dmabuf (GstMppAllocator * allocator,
    GstAllocator * dmabuf_allocator)
{
  GstMppMemory *mem = NULL;
  GstMemory *dma_mem = NULL;

  mem = gst_atomic_queue_pop (allocator->free_queue);
  if (!mem)
    return dma_mem;

  if (mem->dmafd < 0) {
    GST_ERROR_OBJECT (dmabuf_allocator, "Failed to get dmafd");
    return NULL;
  }

  dma_mem = gst_dmabuf_allocator_alloc (dmabuf_allocator, mem->dmafd,
      mem->mem.size);
  gst_mini_object_set_qdata (GST_MINI_OBJECT (dma_mem),
      GST_MPP_MEMORY_QUARK, mem, (GDestroyNotify) gst_memory_unref);

  return dma_mem;
}

static guint
gst_mpp_allocator_ion_buf (GstMppAllocator * allocator, gsize size,
    guint32 count)
{
  MppBufferGroup group;
  MppBuffer temp_buf[VIDEO_MAX_FRAME];

  mpp_buffer_group_get_internal (&group, MPP_BUFFER_TYPE_ION);

  for (gint i = 0; i < count; i++) {
    /*
     * Create MppBuffer from Rockchip Mpp
     * included mvc data
     */
    if (mpp_buffer_get (group, &temp_buf[i], size)) {
      GST_ERROR_OBJECT (allocator, "allocate internal buffer %d failed", i);
      goto error;
    }
  }

  for (gint i = 0; i < count; i++) {
    MppBuffer mpp_buf;
    GstMppMemory *mem = NULL;
    MppBufferInfo commit = { 0, };

    mpp_buf = temp_buf[i];

    commit.type = MPP_BUFFER_TYPE_EXT_DMA;
    commit.fd = dup (mpp_buffer_get_fd (mpp_buf));
    commit.size = mpp_buffer_get_size (mpp_buf);
    commit.index = i;

    /*
     * After this function return success, the inc_buffer_ref_no_lock()
     * in mpp_buffer_create() would increase the internal buffer reference
     * count and remove it from group, remember to put it back to group
     * before future usage
     */
    if (mpp_buffer_import_with_tag (allocator->mpp_mem_pool, &commit,
            &mpp_buf, NULL, __FUNCTION__)) {
      GST_ERROR_OBJECT (allocator, "commit buffer %d failed", i);
      mpp_buffer_put (temp_buf[i]);
      continue;
    }
    mpp_buffer_put (temp_buf[i]);

    mem = _mppmem_new (0, GST_ALLOCATOR (allocator), NULL,
        mpp_buffer_get_size (mpp_buf), 0, 0, mpp_buffer_get_size (mpp_buf),
        NULL, mpp_buffer_get_fd (mpp_buf), mpp_buf);

    gst_atomic_queue_push (allocator->free_queue, mem);
    allocator->mems[i] = mem;
    allocator->count++;
  }

  mpp_buffer_group_put (group);
  return allocator->count;

error:
  {
    allocator->count = 0;
    return 0;
  }
}

static guint
gst_mpp_allocator_drm_buf (GstMppAllocator * allocator, gsize size,
    guint32 count)
{
  MppBufferGroup group;
  MppBuffer temp_buf[VIDEO_MAX_FRAME];

  mpp_buffer_group_get_internal (&group, MPP_BUFFER_TYPE_DRM);

  /* Create DRM buffer from rockchip mpp internally */
  for (gint i = 0; i < count; i++) {
    /*
     * Create MppBuffer from Rockchip Mpp
     * included mvc data
     */
    if (mpp_buffer_get (group, &temp_buf[i], size)) {
      GST_ERROR_OBJECT (allocator, "allocate internal buffer %d failed", i);
      goto error;
    }
  }

  for (gint i = 0; i < count; i++) {
    MppBuffer mpp_buf;
    GstMppMemory *mem = NULL;
    MppBufferInfo commit = { 0, };

    mpp_buf = temp_buf[i];

    commit.type = MPP_BUFFER_TYPE_EXT_DMA;
    commit.fd = dup (mpp_buffer_get_fd (mpp_buf));
    commit.size = mpp_buffer_get_size (mpp_buf);
    commit.index = i;

    /*
     * After this function return success, the inc_buffer_ref_no_lock()
     * in mpp_buffer_create() would increase the internal buffer reference
     * count and remove it from group, remember to put it back to group
     * before future usage
     */
    if (mpp_buffer_import_with_tag (allocator->mpp_mem_pool, &commit,
            &mpp_buf, NULL, __FUNCTION__)) {
      GST_ERROR_OBJECT (allocator, "commit buffer %d failed", i);
      mpp_buffer_put (temp_buf[i]);
      continue;
    }
    mpp_buffer_put (temp_buf[i]);

    mem = _mppmem_new (0, GST_ALLOCATOR (allocator), NULL,
        mpp_buffer_get_size (mpp_buf), 0, 0, mpp_buffer_get_size (mpp_buf),
        NULL, mpp_buffer_get_fd (mpp_buf), mpp_buf);

    gst_atomic_queue_push (allocator->free_queue, mem);
    allocator->mems[i] = mem;
    allocator->count++;
  }

  mpp_buffer_group_put (group);
  return allocator->count;

error:
  {
    allocator->count = 0;
    return 0;
  }
}

gboolean
gst_mpp_allocator_qbuf (GstMppAllocator * allocator, GstMppMemory * mem)
{
  gboolean ret = TRUE;
  g_return_val_if_fail (g_atomic_int_get (&allocator->active), GST_FLOW_ERROR);

  gst_memory_ref (&mem->mem);
  /* Release the internal refcount in mpp */
  if (mpp_buffer_put (mem->mpp_buf)) {
    GST_ERROR_OBJECT (allocator, "failed queueing buffer %i: %s",
        mem->index, g_strerror (errno));
    gst_memory_unref (&mem->mem);
    ret = FALSE;
    goto done;
  }

  GST_LOG_OBJECT (allocator, "queued buffer %i", mem->index);
done:
  return ret;
}

GstFlowReturn
gst_mpp_allocator_dqbuf (GstMppAllocator * allocator, GstMppMemory ** mem_out)
{
  GstMppObject *obj = NULL;
  GstMppMemory *mem = NULL;
  GstFlowReturn res;
  MppFrame mframe = NULL;
  MppBuffer mpp_buf = NULL;
  gint index;

  g_return_val_if_fail (g_atomic_int_get (&allocator->active), GST_FLOW_ERROR);
  obj = allocator->obj;

  /* FIXME only work for decoder */
  res = gst_mpp_object_dec_frame (obj, &mframe);
  if (res != GST_FLOW_OK)
    goto done;

  if (mpp_frame_get_eos (mframe))
    res = GST_FLOW_EOS;

  mpp_buf = mpp_frame_get_buffer (mframe);
  if (!mpp_buf) {
    GST_INFO_OBJECT (allocator, "got eos frame");
    return GST_FLOW_EOS;
  }

  /* TODO: may be re-used for encoder */
  index = mpp_buffer_get_index (mpp_buf);
  if (index < 0)
    goto no_buffer;

  mem = allocator->mems[index];
  if (mem == NULL) {
    GST_ERROR_OBJECT (allocator, "buffer %i was not queued", index);
    return GST_FLOW_ERROR;
  }
  mpp_buffer_inc_ref (mpp_buf);
  /* TODO: may be re-used for encoder */
  mem->data = mframe;

  gst_memory_ref (&(mem->mem));
  *mem_out = mem;

done:
  return res;
  /* ERRORS */
no_buffer:
  {
    GST_ERROR_OBJECT (allocator, "No free buffer found in the pool at index %d",
        index);
    return GST_FLOW_ERROR;
  }
}

guint
gst_mpp_allocator_start (GstMppAllocator * allocator, guint32 count,
    guint32 memory)
{
  guint size = 0;
  guint32 nb = 0;

  g_return_val_if_fail (count != 0, 0);

  GST_OBJECT_LOCK (allocator);
  size = GST_MPP_SIZE (allocator->obj);

  if (g_atomic_int_get (&allocator->active))
    goto already_active;

  mpp_buffer_group_get_external (&allocator->mpp_mem_pool,
      MPP_BUFFER_TYPE_EXT_DMA);
  if (allocator->mpp_mem_pool == NULL)
    goto mpp_mem_pool_error;

  switch (memory) {
    case GST_MPP_IO_ION:
      nb = gst_mpp_allocator_ion_buf (allocator, size, count);
      break;
    case GST_MPP_IO_DRMBUF:
      nb = gst_mpp_allocator_drm_buf (allocator, size, count);
      break;
    case GST_MPP_IO_DMABUF_IMPORT:
      /* It can't fail right ? */
      nb = count;
      break;
  }

  g_atomic_int_set (&allocator->active, TRUE);
done:
  GST_OBJECT_UNLOCK (allocator);

  return nb;
mpp_mem_pool_error:
  {
    GST_ERROR_OBJECT (allocator, "failed to create mpp memory pool");
    goto error;
  }
already_active:
  {
    GST_ERROR_OBJECT (allocator, "allocator already active");
    goto error;
  }
error:
  {
    allocator->count = 0;
    goto done;
  }
}

static void
gst_mpp_allocator_class_init (GstMppAllocatorClass * klass)
{
  GObjectClass *object_class;
  GstAllocatorClass *allocator_class;

  allocator_class = (GstAllocatorClass *) klass;
  object_class = (GObjectClass *) klass;

  allocator_class->alloc = NULL;
  allocator_class->free = gst_mpp_allocator_free;

  object_class->dispose = gst_mpp_allocator_dispose;
  object_class->finalize = gst_mpp_allocator_finalize;

  GST_DEBUG_CATEGORY_INIT (mppallocator_debug, "mppallocator", 0,
      "MPP Allocator");
}

static void
gst_mpp_allocator_init (GstMppAllocator * allocator)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);

  alloc->mem_type = GST_MPP_MEMORY_TYPE;
  alloc->mem_map = (GstMemoryMapFunction) _mppmem_map;
  alloc->mem_unmap = (GstMemoryUnmapFunction) _mppmem_unmap;
  alloc->mem_share = (GstMemoryShareFunction) _mppmem_share;
  alloc->mem_is_span = (GstMemoryIsSpanFunction) _mppmem_is_span;

  allocator->free_queue = gst_atomic_queue_new (VIDEO_MAX_FRAME);

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}
