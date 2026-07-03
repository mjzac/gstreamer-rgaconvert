#include "gstrgadmabufpool.h"
#include "dmaheap.h"

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <unistd.h>

struct _GstRgaDmaBufPool
{
    GstBufferPool parent;

    GstAllocator *dmabuf_alloc;   /* wraps fds into dma-buf GstMemory */
    int           heap_fd;        /* open dma-heap device, -1 if none */
    GstVideoInfo  info;           /* stride-aligned output video info */
    gboolean      add_video_meta;
};

G_DEFINE_FINAL_TYPE (GstRgaDmaBufPool, gst_rga_dmabuf_pool,
    GST_TYPE_BUFFER_POOL)

static const gchar **
gst_rga_dmabuf_pool_get_options (GstBufferPool * pool)
{
    static const gchar *options[] = {
        GST_BUFFER_POOL_OPTION_VIDEO_META,
        GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT,
        NULL,
    };
    (void) pool;
    return options;
}

static gboolean
gst_rga_dmabuf_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
    GstRgaDmaBufPool *self = GST_RGA_DMABUF_POOL (pool);
    GstCaps          *caps = NULL;
    guint             size = 0, min = 0, max = 0;
    GstVideoInfo      info;
    GstVideoAlignment align;
    gint              i;

    if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min, &max)
        || caps == NULL)
        return FALSE;
    if (!gst_video_info_from_caps (&info, caps))
        return FALSE;

    /* RGA requires 16-pixel-aligned plane strides. */
    gst_video_alignment_reset (&align);
    for (i = 0; i < GST_VIDEO_MAX_PLANES; i++)
        align.stride_align[i] = 15;
    gst_video_info_align (&info, &align);

    self->info = info;
    self->add_video_meta = gst_buffer_pool_config_has_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);

    size = MAX (size, (guint) GST_VIDEO_INFO_SIZE (&info));
    gst_buffer_pool_config_set_params (config, caps, size, min, max);

    return GST_BUFFER_POOL_CLASS (gst_rga_dmabuf_pool_parent_class)->set_config
        (pool, config);
}

static GstFlowReturn
gst_rga_dmabuf_pool_alloc_buffer (GstBufferPool * pool, GstBuffer ** buffer,
    GstBufferPoolAcquireParams * params)
{
    GstRgaDmaBufPool *self = GST_RGA_DMABUF_POOL (pool);
    gsize             size = GST_VIDEO_INFO_SIZE (&self->info);
    GstMemory        *mem;
    GstBuffer        *buf;
    int               fd;

    (void) params;

    fd = rga_dmaheap_alloc (self->heap_fd, size);
    if (fd < 0)
        return GST_FLOW_ERROR;

    mem = gst_dmabuf_allocator_alloc (self->dmabuf_alloc, fd, size);
    if (mem == NULL) {
        close (fd);
        return GST_FLOW_ERROR;
    }

    buf = gst_buffer_new ();
    gst_buffer_append_memory (buf, mem);

    if (self->add_video_meta) {
        GstVideoInfo *info = &self->info;
        gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE,
            GST_VIDEO_INFO_FORMAT (info),
            GST_VIDEO_INFO_WIDTH (info), GST_VIDEO_INFO_HEIGHT (info),
            GST_VIDEO_INFO_N_PLANES (info), info->offset, info->stride);
    }

    *buffer = buf;
    return GST_FLOW_OK;
}

static void
gst_rga_dmabuf_pool_finalize (GObject * object)
{
    GstRgaDmaBufPool *self = GST_RGA_DMABUF_POOL (object);

    if (self->dmabuf_alloc != NULL)
        gst_object_unref (self->dmabuf_alloc);
    if (self->heap_fd >= 0)
        close (self->heap_fd);

    G_OBJECT_CLASS (gst_rga_dmabuf_pool_parent_class)->finalize (object);
}

static void
gst_rga_dmabuf_pool_class_init (GstRgaDmaBufPoolClass * klass)
{
    GObjectClass       *gobject_class = G_OBJECT_CLASS (klass);
    GstBufferPoolClass *pool_class    = GST_BUFFER_POOL_CLASS (klass);

    gobject_class->finalize  = gst_rga_dmabuf_pool_finalize;
    pool_class->get_options  = gst_rga_dmabuf_pool_get_options;
    pool_class->set_config   = gst_rga_dmabuf_pool_set_config;
    pool_class->alloc_buffer = gst_rga_dmabuf_pool_alloc_buffer;
}

static void
gst_rga_dmabuf_pool_init (GstRgaDmaBufPool * self)
{
    self->heap_fd      = -1;
    self->dmabuf_alloc = NULL;
    self->add_video_meta = FALSE;
}

GstBufferPool *
gst_rga_dmabuf_pool_new (void)
{
    int heap_fd = rga_dmaheap_open ();
    if (heap_fd < 0)
        return NULL;

    GstRgaDmaBufPool *self = g_object_new (GST_TYPE_RGA_DMABUF_POOL, NULL);
    self->heap_fd      = heap_fd;
    self->dmabuf_alloc = gst_dmabuf_allocator_new ();
    return GST_BUFFER_POOL (self);
}
