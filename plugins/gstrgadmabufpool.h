#ifndef _GST_RGA_DMABUF_POOL_H_
#define _GST_RGA_DMABUF_POOL_H_

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_RGA_DMABUF_POOL (gst_rga_dmabuf_pool_get_type ())
G_DECLARE_FINAL_TYPE (GstRgaDmaBufPool, gst_rga_dmabuf_pool, GST,
    RGA_DMABUF_POOL, GstBufferPool)

/*
 * Create a buffer pool that hands out uncached dma-heap-backed, CPU-mappable
 * dma-buf buffers with 16-pixel-aligned strides. Returns NULL if no dma-heap
 * device could be opened (caller should fall back to a non-dma-buf path).
 */
GstBufferPool *gst_rga_dmabuf_pool_new (void);

G_END_DECLS

#endif /* _GST_RGA_DMABUF_POOL_H_ */
