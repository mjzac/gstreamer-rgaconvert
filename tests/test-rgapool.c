#include "gstrgadmabufpool.h"

#include <gst/gst.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <stdio.h>

int
main (int argc, char **argv)
{
    gst_init (&argc, &argv);

    GstBufferPool *pool = gst_rga_dmabuf_pool_new ();
    if (pool == NULL) {
        fprintf (stderr, "FAIL: gst_rga_dmabuf_pool_new (no dma-heap?)\n");
        return 1;
    }

    /* width 1281 -> byte stride 1281*4 = 5124, which the pool must round up
     * to the next multiple of 16 (5136). 1281 is deliberately NOT 16-aligned
     * so this exercises the pool's stride-alignment path. */
    GstCaps *caps =
        gst_caps_from_string ("video/x-raw,format=BGRx,width=1281,height=720");
    GstVideoInfo info;
    gst_video_info_from_caps (&info, caps);

    GstStructure *config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, caps,
        GST_VIDEO_INFO_SIZE (&info), 2, 0);
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
    if (!gst_buffer_pool_set_config (pool, config)) {
        fprintf (stderr, "FAIL: set_config\n");
        return 1;
    }
    if (!gst_buffer_pool_set_active (pool, TRUE)) {
        fprintf (stderr, "FAIL: set_active\n");
        return 1;
    }

    GstBuffer *buf = NULL;
    if (gst_buffer_pool_acquire_buffer (pool, &buf, NULL) != GST_FLOW_OK
        || buf == NULL) {
        fprintf (stderr, "FAIL: acquire_buffer\n");
        return 1;
    }

    GstMemory *mem = gst_buffer_peek_memory (buf, 0);
    if (!gst_is_dmabuf_memory (mem)) {
        fprintf (stderr, "FAIL: buffer memory is not dma-buf\n");
        return 1;
    }
    GstVideoMeta *vmeta = gst_buffer_get_video_meta (buf);
    if (vmeta == NULL) {
        fprintf (stderr, "FAIL: buffer has no GstVideoMeta\n");
        return 1;
    }
    /* 1281 * 4 = 5124 bytes, rounded up to a multiple of 16 = 5136. */
    if (vmeta->stride[0] != 5136) {
        fprintf (stderr, "FAIL: stride[0]=%d, expected 5136 (16-aligned)\n",
            vmeta->stride[0]);
        return 1;
    }

    gst_buffer_unref (buf);
    gst_buffer_pool_set_active (pool, FALSE);
    gst_object_unref (pool);
    gst_caps_unref (caps);
    printf ("PASS\n");
    return 0;
}
