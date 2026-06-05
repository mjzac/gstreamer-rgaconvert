#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/allocators/gstdmabuf.h>
#include <stdio.h>

int
main (int argc, char **argv)
{
    gst_init (&argc, &argv);

    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch (
        "videotestsrc num-buffers=1 ! "
        "video/x-raw,format=NV12,width=1280,height=720 ! "
        "rgaconvert ! "
        "video/x-raw,format=BGRx,width=1280,height=720 ! "
        "appsink name=sink sync=false", &err);
    if (pipeline == NULL) {
        fprintf (stderr, "FAIL: parse_launch: %s\n", err ? err->message : "?");
        return 1;
    }

    GstElement *sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
    gst_element_set_state (pipeline, GST_STATE_PLAYING);

    GstSample *sample = gst_app_sink_pull_sample (GST_APP_SINK (sink));
    if (sample == NULL) {
        fprintf (stderr, "FAIL: no sample (pipeline did not produce output)\n");
        return 1;
    }

    GstBuffer *buf = gst_sample_get_buffer (sample);
    GstMemory *mem = gst_buffer_peek_memory (buf, 0);
    int ok = gst_is_dmabuf_memory (mem);

    gst_sample_unref (sample);
    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (sink);
    gst_object_unref (pipeline);

    if (!ok) {
        fprintf (stderr, "FAIL: appsink buffer is not dma-buf memory\n");
        return 1;
    }
    printf ("PASS\n");
    return 0;
}
