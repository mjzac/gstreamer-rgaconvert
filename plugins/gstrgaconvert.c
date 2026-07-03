/* GStreamer
 * Copyright (C) 2021 FIXME <fixme@example.com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstrgaconvert
 *
 * The rgaconvert element uses the Rockchip RGA 2D hardware accelerator (via the
 * librga im2d API) to scale, colour-convert, rotate and flip raw video frames.
 * Scaling and format conversion are driven by caps negotiation; the image
 * orientation is controlled through the standard
 * #GstVideoDirection:video-direction property (the same property videoflip
 * uses, so rgaconvert works as a hardware drop-in replacement for it).
 *
 * <refsect2>
 * <title>Example launch lines</title>
 * |[
 * gst-launch-1.0 -v fakesrc ! video/x-raw,format=NV12,width=1920,height=1080 ! rgaconvert ! video/x-raw,format=RGBA,width=640,height=480 ! fakesink
 * ]|
 * convert 1920x1080 ---> 640x480 and NV12 ---> RGBA .
 * |[
 * gst-launch-1.0 -v videotestsrc ! video/x-raw,format=NV12,width=1280,height=720 ! rgaconvert video-direction=90r ! autovideosink
 * ]|
 * rotate the stream 90 degrees clockwise (the output size 720x1280 is chosen
 * automatically; 90l, 180 and the diagonal flips ul-lr / ur-ll are also
 * supported).
 * |[
 * gst-launch-1.0 -v videotestsrc ! rgaconvert video-direction=horiz ! autovideosink
 * ]|
 * mirror the stream horizontally. With video-direction=auto the orientation
 * follows the image-orientation tag carried by the stream.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstrgaconvert.h"
#include "gstrgadmabufpool.h"
#include "rga/im2d.h"
#include <gst/allocators/gstdmabuf.h>
#include <gst/gst.h>
#include <gst/video/gstvideofilter.h>
#include <gst/video/gstvideopool.h>
#include <gst/video/video.h>

GST_DEBUG_CATEGORY_STATIC(gst_rga_convert_debug_category);
#define GST_CAT_DEFAULT gst_rga_convert_debug_category

#define GST_CASE_RETURN(a, b) \
    case a:                   \
        return b

enum
{
    PROP_0,
    PROP_VIDEO_DIRECTION,
};

/* prototypes */

static void gst_rga_convert_set_property(GObject *object, guint prop_id,
                                         const GValue *value, GParamSpec *pspec);
static void gst_rga_convert_get_property(GObject *object, guint prop_id,
                                         GValue *value, GParamSpec *pspec);

static gboolean gst_rga_convert_sink_event(GstBaseTransform *trans,
                                           GstEvent *event);

static GstCaps *gst_rga_convert_transform_caps(
    GstBaseTransform *trans, GstPadDirection direction, GstCaps *caps,
    GstCaps *filter);

static GstCaps *gst_rga_convert_fixate_caps(
    GstBaseTransform *trans, GstPadDirection direction, GstCaps *caps,
    GstCaps *othercaps);

static gboolean gst_rga_convert_decide_allocation(GstBaseTransform *trans,
                                                  GstQuery *query);

static gboolean gst_rga_convert_propose_allocation(GstBaseTransform *trans,
                                                   GstQuery *decide_query, GstQuery *query);

static gboolean gst_rga_convert_set_info(GstVideoFilter *filter,
                                         GstCaps *incaps, GstVideoInfo *in_info, GstCaps *outcaps, GstVideoInfo *out_info);

static GstFlowReturn gst_rga_convert_transform_frame(GstVideoFilter *filter,
                                                     GstVideoFrame *inframe, GstVideoFrame *outframe);

/* pad templates */

#define RGA_FORMATS \
    "{ I420, YV12, NV12, NV21, Y42B, NV16, NV61, RGB16, RGB15, BGR, RGB, BGRA, RGBA, BGRx, RGBx }"

/*
 * Both pads advertise plain video/x-raw (system memory) *and* the
 * video/x-raw(memory:DMABuf) caps feature. The DMABuf variant lets dma-buf
 * producers that signal their memory at the caps layer (e.g. mppvideodec,
 * mppjpegdec) link to our sink pad, and lets us hand RGA's dma-buf output
 * zero-copy to a dma-buf consumer (e.g. kmssink) on our src pad. RGA can import
 * from / write to either memory type, so the feature is negotiated
 * independently on each pad (see gst_rga_convert_transform_caps).
 */
#define VIDEO_SRC_CAPS                                                          \
    "video/x-raw(" GST_CAPS_FEATURE_MEMORY_DMABUF "), "                         \
    "format = (string) " RGA_FORMATS ", "                                       \
    "width = (int) [ 1, 4096 ] ,"                                               \
    "height = (int) [ 1, 4096 ] ,"                                              \
    "framerate = (fraction) [ 0, max ] ;"                                       \
    "video/x-raw, "                                                             \
    "format = (string) " RGA_FORMATS ", "                                       \
    "width = (int) [ 1, 4096 ] ,"                                               \
    "height = (int) [ 1, 4096 ] ,"                                              \
    "framerate = (fraction) [ 0, max ]"

#define VIDEO_SINK_CAPS                                                         \
    "video/x-raw(" GST_CAPS_FEATURE_MEMORY_DMABUF "), "                         \
    "format = (string) " RGA_FORMATS ", "                                       \
    "width = (int) [ 1, 8192 ] ,"                                               \
    "height = (int) [ 1, 8192 ] ,"                                              \
    "framerate = (fraction) [ 0, max ] ;"                                       \
    "video/x-raw, "                                                             \
    "format = (string) " RGA_FORMATS ", "                                       \
    "width = (int) [ 1, 8192 ] ,"                                               \
    "height = (int) [ 1, 8192 ] ,"                                              \
    "framerate = (fraction) [ 0, max ]"

/* class initialization */

/* The GstVideoDirection interface has no vfuncs; implementing it only makes
 * the standard "video-direction" property available for overriding. */
G_DEFINE_TYPE_WITH_CODE(GstRgaConvert, gst_rga_convert, GST_TYPE_VIDEO_FILTER,
                        G_IMPLEMENT_INTERFACE(GST_TYPE_VIDEO_DIRECTION, NULL)
                        GST_DEBUG_CATEGORY_INIT(gst_rga_convert_debug_category, "rgaconvert", 0,
                                                "debug category for rgaconvert element"));

static void
gst_rga_convert_class_init(GstRgaConvertClass *klass)
{
    GObjectClass *         gobject_class        = G_OBJECT_CLASS(klass);
    GstBaseTransformClass *base_transform_class = GST_BASE_TRANSFORM_CLASS(klass);
    GstVideoFilterClass *  video_filter_class   = GST_VIDEO_FILTER_CLASS(klass);

    /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
    gst_element_class_add_pad_template(GST_ELEMENT_CLASS(klass),
                                       gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                                                            gst_caps_from_string(VIDEO_SRC_CAPS)));
    gst_element_class_add_pad_template(GST_ELEMENT_CLASS(klass),
                                       gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                                                            gst_caps_from_string(VIDEO_SINK_CAPS)));

    gst_element_class_set_static_metadata(GST_ELEMENT_CLASS(klass),
                                          "Rockchip rga hardware convert", "Filter/Converter/Video/Scaler",
                                          "Scale, color-convert, rotate and flip video streams via the Rockchip RGA 2D hardware (im2d API)",
                                          "http://github.com/higithubhi/gstreamer-rgaconvert");

    gobject_class->set_property = gst_rga_convert_set_property;
    gobject_class->get_property = gst_rga_convert_get_property;

    g_object_class_override_property(gobject_class, PROP_VIDEO_DIRECTION,
                                     "video-direction");

    /* A rotation/flip request must run through the hardware even when the
     * input and output caps are identical, so the same-caps passthrough
     * optimisation cannot be used. */
    base_transform_class->passthrough_on_same_caps = FALSE;

    base_transform_class->sink_event         = GST_DEBUG_FUNCPTR(gst_rga_convert_sink_event);
    base_transform_class->transform_caps     = GST_DEBUG_FUNCPTR(gst_rga_convert_transform_caps);
    base_transform_class->fixate_caps        = GST_DEBUG_FUNCPTR(gst_rga_convert_fixate_caps);
    base_transform_class->decide_allocation  = GST_DEBUG_FUNCPTR(gst_rga_convert_decide_allocation);
    base_transform_class->propose_allocation = GST_DEBUG_FUNCPTR(gst_rga_convert_propose_allocation);

    video_filter_class->set_info        = GST_DEBUG_FUNCPTR(gst_rga_convert_set_info);
    video_filter_class->transform_frame = GST_DEBUG_FUNCPTR(gst_rga_convert_transform_frame);
}

/*
 * Record a new orientation request (from the property or from an
 * image-orientation tag) and resolve the method that is actually applied.
 * When the effective orientation changes, ask the base class to renegotiate
 * the source caps since a 90/270 degree method changes the output size.
 */
static void
gst_rga_convert_set_method(GstRgaConvert *rgaconvert,
                           GstVideoOrientationMethod method, gboolean from_tag)
{
    GstVideoOrientationMethod active;

    GST_OBJECT_LOCK(rgaconvert);

    if (method == GST_VIDEO_ORIENTATION_CUSTOM)
    {
        GST_WARNING_OBJECT(rgaconvert,
                           "unsupported custom orientation, using identity");
        method = GST_VIDEO_ORIENTATION_IDENTITY;
    }

    if (from_tag)
        rgaconvert->tag_method = method;
    else
        rgaconvert->video_direction = method;

    active = rgaconvert->video_direction == GST_VIDEO_ORIENTATION_AUTO
                 ? rgaconvert->tag_method
                 : rgaconvert->video_direction;

    if (active == rgaconvert->active_method)
    {
        GST_OBJECT_UNLOCK(rgaconvert);
        return;
    }

    GST_DEBUG_OBJECT(rgaconvert, "video direction %d -> %d",
                     rgaconvert->active_method, active);
    rgaconvert->active_method = active;
    GST_OBJECT_UNLOCK(rgaconvert);

    gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(rgaconvert));
}

static void
gst_rga_convert_set_property(GObject *object, guint prop_id,
                             const GValue *value, GParamSpec *pspec)
{
    GstRgaConvert *rgaconvert = GST_RGA_CONVERT(object);

    switch (prop_id)
    {
    case PROP_VIDEO_DIRECTION:
        gst_rga_convert_set_method(rgaconvert, g_value_get_enum(value), FALSE);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
gst_rga_convert_get_property(GObject *object, guint prop_id,
                             GValue *value, GParamSpec *pspec)
{
    GstRgaConvert *rgaconvert = GST_RGA_CONVERT(object);

    switch (prop_id)
    {
    case PROP_VIDEO_DIRECTION:
        GST_OBJECT_LOCK(rgaconvert);
        g_value_set_enum(value, rgaconvert->video_direction);
        GST_OBJECT_UNLOCK(rgaconvert);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

/*
 * Map a GST_TAG_IMAGE_ORIENTATION string to the orientation method that fixes
 * it for display. Same table as gst_video_orientation_from_tag(), inlined so
 * the plugin keeps building against GStreamer < 1.20. Per the tag definition
 * "flip-rotate-N" means mirror horizontally, then rotate N degrees clockwise:
 * flip-rotate-90 is the ur-ll transpose (EXIF 7), flip-rotate-270 the ul-lr
 * transpose (EXIF 5).
 */
static gboolean
gst_rga_convert_orientation_from_tag(const gchar *orientation,
                                     GstVideoOrientationMethod *method)
{
    static const struct
    {
        const gchar *             tag;
        GstVideoOrientationMethod method;
    } table[] = {
        {"rotate-0", GST_VIDEO_ORIENTATION_IDENTITY},
        {"rotate-90", GST_VIDEO_ORIENTATION_90R},
        {"rotate-180", GST_VIDEO_ORIENTATION_180},
        {"rotate-270", GST_VIDEO_ORIENTATION_90L},
        {"flip-rotate-0", GST_VIDEO_ORIENTATION_HORIZ},
        {"flip-rotate-90", GST_VIDEO_ORIENTATION_UR_LL},
        {"flip-rotate-180", GST_VIDEO_ORIENTATION_VERT},
        {"flip-rotate-270", GST_VIDEO_ORIENTATION_UL_LR},
    };

    for (gsize i = 0; i < G_N_ELEMENTS(table); i++)
    {
        if (g_str_equal(orientation, table[i].tag))
        {
            *method = table[i].method;
            return TRUE;
        }
    }

    return FALSE;
}

/* Track image-orientation tags so video-direction=auto can follow them. The
 * event is always chained up, so the tag still travels downstream. */
static gboolean
gst_rga_convert_sink_event(GstBaseTransform *trans, GstEvent *event)
{
    GstRgaConvert *rgaconvert = GST_RGA_CONVERT(trans);

    if (GST_EVENT_TYPE(event) == GST_EVENT_TAG)
    {
        GstTagList *taglist;
        gchar *     orientation;

        gst_event_parse_tag(event, &taglist);
        if (gst_tag_list_get_string(taglist, GST_TAG_IMAGE_ORIENTATION,
                                    &orientation))
        {
            GstVideoOrientationMethod method;

            if (gst_rga_convert_orientation_from_tag(orientation, &method))
                gst_rga_convert_set_method(rgaconvert, method, TRUE);
            else
                GST_WARNING_OBJECT(rgaconvert,
                                   "unknown image-orientation tag \"%s\"", orientation);
            g_free(orientation);
        }
    }

    return GST_BASE_TRANSFORM_CLASS(gst_rga_convert_parent_class)
        ->sink_event(trans, event);
}

/*
 * Translate the active orientation method into RGA im2d usage flags. RGA's
 * IM_HAL_TRANSFORM_ROT_* follows the Android HAL convention where ROT_90 is a
 * clockwise rotation. The two diagonal transposes compose a rotation with a
 * mirror in a single blit; RGA applies the rotation first and then the mirror
 * (verified against videoflip output on RK35xx hardware, librga 1.10.1).
 */
static int
gst_rga_convert_build_usage(GstVideoOrientationMethod method)
{
    switch (method)
    {
        GST_CASE_RETURN(GST_VIDEO_ORIENTATION_90R, IM_SYNC | IM_HAL_TRANSFORM_ROT_90);
        GST_CASE_RETURN(GST_VIDEO_ORIENTATION_180, IM_SYNC | IM_HAL_TRANSFORM_ROT_180);
        GST_CASE_RETURN(GST_VIDEO_ORIENTATION_90L, IM_SYNC | IM_HAL_TRANSFORM_ROT_270);
        GST_CASE_RETURN(GST_VIDEO_ORIENTATION_HORIZ, IM_SYNC | IM_HAL_TRANSFORM_FLIP_H);
        GST_CASE_RETURN(GST_VIDEO_ORIENTATION_VERT, IM_SYNC | IM_HAL_TRANSFORM_FLIP_V);
        GST_CASE_RETURN(GST_VIDEO_ORIENTATION_UL_LR, IM_SYNC | IM_HAL_TRANSFORM_ROT_90 | IM_HAL_TRANSFORM_FLIP_H);
        GST_CASE_RETURN(GST_VIDEO_ORIENTATION_UR_LL, IM_SYNC | IM_HAL_TRANSFORM_ROT_90 | IM_HAL_TRANSFORM_FLIP_V);
    default: /* identity (AUTO is resolved before it gets here) */
        return IM_SYNC;
    }
}

/* 90/270 degree rotations and the diagonal transposes swap width and height. */
static gboolean
gst_rga_convert_method_swaps_wh(GstVideoOrientationMethod method)
{
    return method == GST_VIDEO_ORIENTATION_90R
        || method == GST_VIDEO_ORIENTATION_90L
        || method == GST_VIDEO_ORIENTATION_UL_LR
        || method == GST_VIDEO_ORIENTATION_UR_LL;
}

/* Read the orientation actually in effect (never AUTO). */
static GstVideoOrientationMethod
gst_rga_convert_get_active_method(GstRgaConvert *rgaconvert)
{
    GstVideoOrientationMethod method;

    GST_OBJECT_LOCK(rgaconvert);
    method = rgaconvert->active_method;
    GST_OBJECT_UNLOCK(rgaconvert);

    return method;
}

static RgaSURF_FORMAT gst_gst_format_to_rga_format(GstVideoFormat format)
{
    switch (format)
    {
        GST_CASE_RETURN(GST_VIDEO_FORMAT_I420, RK_FORMAT_YCbCr_420_P);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_YV12, RK_FORMAT_YCrCb_420_P);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_NV12, RK_FORMAT_YCbCr_420_SP);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_NV21, RK_FORMAT_YCrCb_420_SP);
#ifdef HAVE_NV12_10LE40
        GST_CASE_RETURN(
            GST_VIDEO_FORMAT_NV12_10LE40, RK_FORMAT_YCbCr_420_SP_10B);
#endif
        GST_CASE_RETURN(GST_VIDEO_FORMAT_Y42B, RK_FORMAT_YCbCr_422_P);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_NV16, RK_FORMAT_YCbCr_422_SP);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_NV61, RK_FORMAT_YCrCb_422_SP);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_RGB16, RK_FORMAT_RGB_565);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_RGB15, RK_FORMAT_RGBA_5551);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_BGR, RK_FORMAT_BGR_888);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_RGB, RK_FORMAT_RGB_888);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_BGRA, RK_FORMAT_BGRA_8888);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_RGBA, RK_FORMAT_RGBA_8888);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_BGRx, RK_FORMAT_BGRX_8888);
        GST_CASE_RETURN(GST_VIDEO_FORMAT_RGBx, RK_FORMAT_RGBX_8888);
    default:
        return RK_FORMAT_UNKNOWN;
    }
}

/* A GstVideoFrame imported into the RGA driver as an im2d buffer. */
typedef struct
{
    rga_buffer_handle_t handle;   /* RGA buffer handle, 0 when not imported */
    rga_buffer_t        buffer;   /* im2d buffer descriptor */
    GstMapInfo          map_info; /* CPU mapping, valid when 'mapped' is TRUE */
    gboolean            mapped;
} GstRgaFrame;

/*
 * Bytes per pixel for packed/RGB formats, or 1 for YUV formats whose luma
 * stride is already expressed in samples. Returns 0 for unsupported formats.
 */
static gint
gst_rga_convert_pixel_stride(RgaSURF_FORMAT format)
{
    switch (format)
    {
    case RK_FORMAT_RGBX_8888:
    case RK_FORMAT_BGRX_8888:
    case RK_FORMAT_RGBA_8888:
    case RK_FORMAT_BGRA_8888:
        return 4;
    case RK_FORMAT_RGB_888:
    case RK_FORMAT_BGR_888:
        return 3;
    case RK_FORMAT_RGBA_5551:
    case RK_FORMAT_RGB_565:
        return 2;
    case RK_FORMAT_YCbCr_420_SP_10B:
    case RK_FORMAT_YCbCr_422_SP:
    case RK_FORMAT_YCrCb_422_SP:
    case RK_FORMAT_YCbCr_422_P:
    case RK_FORMAT_YCrCb_422_P:
    case RK_FORMAT_YCbCr_420_SP:
    case RK_FORMAT_YCrCb_420_SP:
    case RK_FORMAT_YCbCr_420_P:
    case RK_FORMAT_YCrCb_420_P:
        return 1;
    default:
        return 0;
    }
}

/*
 * Import a GstVideoFrame into the RGA driver, preferring a zero-copy dma-buf
 * import and falling back to a CPU mapping + virtual-address import. On success
 * 'rga_frame' must later be passed to gst_rga_frame_release().
 */
static gboolean
gst_rga_frame_import(GstRgaFrame *rga_frame, GstVideoFrame *frame, GstMapFlags map_flags)
{
    RgaSURF_FORMAT format = gst_gst_format_to_rga_format(GST_VIDEO_FRAME_FORMAT(frame));
    gint pixel_stride     = gst_rga_convert_pixel_stride(format);

    if (pixel_stride == 0)
        return FALSE;

    gint width   = GST_VIDEO_FRAME_WIDTH(frame);
    gint height  = GST_VIDEO_FRAME_HEIGHT(frame);
    gint hstride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
    gint vstride = GST_VIDEO_FRAME_N_PLANES(frame) == 1
                       ? GST_VIDEO_INFO_HEIGHT(&frame->info)
                       : GST_VIDEO_INFO_PLANE_OFFSET(&frame->info, 1) / hstride;

    /* im2d expresses strides in pixels; convert the luma byte stride for
     * packed/RGB formats. */
    if (hstride / pixel_stride >= width)
        hstride /= pixel_stride;

    /* RGA requires YUV image rectangles to be aligned to 2 pixels. */
    if (pixel_stride == 1)
    {
        width  &= ~1;
        height &= ~1;
    }

    GstBuffer *buf = frame->buffer;
    gint       fd  = -1;

    if (gst_buffer_n_memory(buf) == 1)
    {
        GstMemory *mem = gst_buffer_peek_memory(buf, 0);
        gsize      offset;

        if (gst_is_dmabuf_memory(mem))
        {
            gst_memory_get_sizes(mem, &offset, NULL);
            if (!offset)
                fd = gst_dmabuf_memory_get_fd(mem);
        }
    }

    im_handle_param_t param = {
        .width  = (uint32_t)hstride,
        .height = (uint32_t)vstride,
        .format = (uint32_t)format,
    };

    if (fd > 0)
    {
        rga_frame->handle = importbuffer_fd(fd, &param);
        GST_LOG("rga import: dma-buf fd %d (zero-copy)", fd);
    }
    else
    {
        if (!gst_buffer_map(buf, &rga_frame->map_info, map_flags))
            return FALSE;
        rga_frame->mapped = TRUE;
        rga_frame->handle = importbuffer_virtualaddr(rga_frame->map_info.data, &param);
        GST_LOG("rga import: CPU virtual address (no dma-buf, slow path)");
    }

    if (rga_frame->handle == 0)
        return FALSE;

    rga_frame->buffer =
        wrapbuffer_handle(rga_frame->handle, width, height, format, hstride, vstride);

    return TRUE;
}

static void
gst_rga_frame_release(GstRgaFrame *rga_frame, GstVideoFrame *frame)
{
    if (rga_frame->handle != 0)
        releasebuffer_handle(rga_frame->handle);
    if (rga_frame->mapped)
        gst_buffer_unmap(frame->buffer, &rga_frame->map_info);
}

/*
 * Append 'structure' to 'caps' tagged with the named caps feature (NULL for the
 * default system-memory feature), skipping it when an equal-or-broader
 * structure is already present. The caller retains ownership of 'structure'.
 */
static void
gst_rga_convert_append_with_feature(GstCaps *caps, const GstStructure *structure,
                                    const gchar *feature_name)
{
    GstCapsFeatures *features =
        feature_name ? gst_caps_features_new(feature_name, NULL) : NULL;

    if (gst_caps_is_subset_structure_full(caps, structure, features))
    {
        if (features)
            gst_caps_features_free(features);
        return;
    }

    gst_caps_append_structure_full(caps, gst_structure_copy(structure), features);
}

static GstCaps *gst_rga_convert_transform_caps(
    GstBaseTransform *trans, GstPadDirection direction, GstCaps *caps,
    GstCaps *filter)
{
    GST_DEBUG_OBJECT(trans,
                     "transform direction %s : caps=%" GST_PTR_FORMAT "    filter=%" GST_PTR_FORMAT,
                     direction == GST_PAD_SINK ? "sink" : "src",
                     caps,
                     filter);

    GstCaps *ret;
    GstStructure *structure;
    GstCapsFeatures *features;
    gint i, n;

    ret = gst_caps_new_empty();
    n = gst_caps_get_size(caps);
    for (i = 0; i < n; i++)
    {
        structure = gst_caps_get_structure(caps, i);
        features = gst_caps_get_features(caps, i);

        /* make copy */
        structure = gst_structure_copy(structure);

        if (direction == GST_PAD_SRC)
        {
            /* caps are on the src (output) pad; describe the sink (input)
             * pad here. RGA input max is 8192. */
            gst_structure_set(structure,
                              "width",
                              GST_TYPE_INT_RANGE,
                              1,
                              8192,
                              "height",
                              GST_TYPE_INT_RANGE,
                              1,
                              8192,
                              NULL);
        } else
        {
            /* caps are on the sink (input) pad; describe the src (output)
             * pad here. RGA output max is 4096. */
            gst_structure_set(structure,
                              "width",
                              GST_TYPE_INT_RANGE,
                              1,
                              4096,
                              "height",
                              GST_TYPE_INT_RANGE,
                              1,
                              4096,
                              NULL);
        }

        if (gst_caps_features_is_any(features))
        {
            /* Wildcard features: keep the structure (and its format) verbatim. */
            gst_caps_append_structure_full(ret, structure, gst_caps_features_copy(features));
            continue;
        }

        /* RGA converts pixel format and can import from / write to either system
         * memory or a dma-buf, so the other pad's format and memory type are not
         * constrained by this structure. Drop the format and offer both the
         * system-memory and the dma-buf variants. */
        gst_structure_remove_fields(structure, "format", "colorimetry", "chroma-site", NULL);

        gst_rga_convert_append_with_feature(ret, structure, NULL);
        gst_rga_convert_append_with_feature(ret, structure, GST_CAPS_FEATURE_MEMORY_DMABUF);
        gst_structure_free(structure);
    }

    if (filter) {
        GstCaps *intersection;

        intersection =
            gst_caps_intersect_full(filter, ret, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(ret);
        ret = intersection;
    }

    GST_DEBUG_OBJECT(trans, "returning caps: %" GST_PTR_FORMAT, ret);

    return ret;
}

/*
 * Choose a default output resolution when downstream leaves it open. For a
 * plain converter that is just the input size, but a 90/270 degree method
 * (including the diagonal transposes) swaps width and height so the default
 * output must be swapped too (e.g. 1920x1080 rotated 90 defaults to
 * 1080x1920). An explicit downstream caps filter still wins via
 * gst_structure_fixate_field_nearest_int().
 */
static GstCaps *
gst_rga_convert_fixate_caps(GstBaseTransform *trans, GstPadDirection direction,
                            GstCaps *caps, GstCaps *othercaps)
{
    GstRgaConvert *rgaconvert = GST_RGA_CONVERT(trans);

    othercaps = gst_caps_make_writable(othercaps);

    /* Only steer the default size when transforming a fixed input (sink)
     * towards the output (src). */
    if (direction == GST_PAD_SINK)
    {
        GstStructure *ins = gst_caps_get_structure(caps, 0);
        gint          in_w = 0, in_h = 0;

        if (gst_structure_get_int(ins, "width", &in_w)
            && gst_structure_get_int(ins, "height", &in_h))
        {
            gint target_w = in_w;
            gint target_h = in_h;

            if (gst_rga_convert_method_swaps_wh(
                    gst_rga_convert_get_active_method(rgaconvert)))
            {
                target_w = in_h;
                target_h = in_w;
            }

            GstStructure *outs = gst_caps_get_structure(othercaps, 0);
            gst_structure_fixate_field_nearest_int(outs, "width", target_w);
            gst_structure_fixate_field_nearest_int(outs, "height", target_h);
        }
    }

    /* Fixate any remaining fields (format, framerate, ...) to defaults. */
    othercaps = gst_caps_fixate(othercaps);

    GST_DEBUG_OBJECT(trans, "fixated to: %" GST_PTR_FORMAT, othercaps);

    return othercaps;
}

/*
 * Install a self-allocated, dma-heap-backed dma-buf pool as the output pool so
 * RGA writes converted frames directly into dma-buf (zero-copy towards a
 * dma-buf-aware or CPU-mapping consumer). The pool also applies the 16-pixel
 * stride alignment RGA requires. When no dma-heap is available (or the pool
 * fails to configure) we chain up to the parent, which falls back to a
 * CPU-mapped output pool.
 *
 * Note: on the success path we deliberately do NOT chain up to the parent.
 * This element must own a dma-buf output pool; chaining up would install/keep
 * a CPU pool, and downstream-offered allocators are intentionally ignored.
 */
static gboolean
gst_rga_convert_decide_allocation (GstBaseTransform *trans, GstQuery *query)
{
    GstCaps       *outcaps = NULL;
    GstBufferPool *pool    = NULL;
    GstStructure  *config;
    GstVideoInfo   info;
    guint          size = 0, min = 0, max = 0;

    gst_query_parse_allocation (query, &outcaps, NULL);
    if (outcaps == NULL || !gst_video_info_from_caps (&info, outcaps))
        return GST_BASE_TRANSFORM_CLASS (gst_rga_convert_parent_class)
            ->decide_allocation (trans, query);

    pool = gst_rga_dmabuf_pool_new ();
    if (pool == NULL) {
        GST_WARNING_OBJECT (trans, "no dma-heap available; falling back to "
                                   "non-zero-copy output path");
        return GST_BASE_TRANSFORM_CLASS (gst_rga_convert_parent_class)
            ->decide_allocation (trans, query);
    }

    /* Honour downstream's requested buffer counts if it offered a pool. */
    if (gst_query_get_n_allocation_pools (query) > 0)
        gst_query_parse_nth_allocation_pool (query, 0, NULL, NULL, &min, &max);
    if (min < 2)
        min = 2;

    size = GST_VIDEO_INFO_SIZE (&info);

    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, outcaps, size, min, max);
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
    if (!gst_buffer_pool_set_config (pool, config)) {
        GST_WARNING_OBJECT (trans, "failed to configure dma-buf pool; "
                                   "falling back to non-zero-copy output path");
        gst_object_unref (pool);
        return GST_BASE_TRANSFORM_CLASS (gst_rga_convert_parent_class)
            ->decide_allocation (trans, query);
    }

    /* Re-read the (possibly grown) size the pool settled on. */
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_params (config, NULL, &size, NULL, NULL);
    gst_structure_free (config);

    if (gst_query_get_n_allocation_pools (query) > 0)
        gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
    else
        gst_query_add_allocation_pool (query, pool, size, min, max);

    if (!gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL))
        gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

    GST_DEBUG_OBJECT (trans, "using dma-buf output pool (size %u, min %u)",
        size, min);

    gst_object_unref (pool);
    return TRUE;
}

/*
 * Tell upstream we can attach/accept a GstVideoMeta. A dma-buf producing source
 * (e.g. a camera in io-mode=dmabuf) then supplies stride metadata that
 * gst_rga_frame_import() uses to import the buffer zero-copy via importbuffer_fd.
 */
static gboolean
gst_rga_convert_propose_allocation (GstBaseTransform *trans,
                                    GstQuery *decide_query, GstQuery *query)
{
    if (!GST_BASE_TRANSFORM_CLASS (gst_rga_convert_parent_class)
             ->propose_allocation (trans, decide_query, query))
        return FALSE;

    if (!gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL))
        gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);

    return TRUE;
}

static void
gst_rga_convert_init(GstRgaConvert *rgaconvert)
{
    rgaconvert->video_direction = GST_VIDEO_ORIENTATION_IDENTITY;
    rgaconvert->active_method   = GST_VIDEO_ORIENTATION_IDENTITY;
    rgaconvert->tag_method      = GST_VIDEO_ORIENTATION_IDENTITY;
}

static gboolean
gst_rga_convert_set_info(GstVideoFilter *filter, GstCaps *incaps,
                         GstVideoInfo *in_info, GstCaps *outcaps, GstVideoInfo *out_info)
{
    GstRgaConvert *rgaconvert = GST_RGA_CONVERT(filter);
    GST_DEBUG_OBJECT(rgaconvert, "set_info");

    GstVideoFormat in_format  = GST_VIDEO_INFO_FORMAT(in_info);
    GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT(out_info);

    if (gst_gst_format_to_rga_format(in_format) == RK_FORMAT_UNKNOWN
        || gst_gst_format_to_rga_format(out_format) == RK_FORMAT_UNKNOWN)
    {
        GST_INFO_OBJECT(filter, "don't support format. in format=%d,out format=%d", in_format, out_format);
        return FALSE;
    }
    return TRUE;
}

/* transform */
static GstFlowReturn
gst_rga_convert_transform_frame(GstVideoFilter *filter, GstVideoFrame *inframe,
                                GstVideoFrame *outframe)
{
    GstRgaConvert *rgaconvert = GST_RGA_CONVERT(filter);
    GstRgaFrame    src         = {0,};
    GstRgaFrame    dst         = {0,};
    GstFlowReturn  flow        = GST_FLOW_OK;
    im_rect        empty_rect  = {0,};
    rga_buffer_t   empty_pat   = {0,};
    int            usage;
    IM_STATUS      status;

    GST_DEBUG_OBJECT(rgaconvert, "transform_frame");

    if (!gst_rga_frame_import(&src, inframe, GST_MAP_READ))
    {
        GST_WARNING_OBJECT(filter, "failed to import source frame into rga");
        flow = GST_FLOW_ERROR;
        goto out;
    }

    if (!gst_rga_frame_import(&dst, outframe, GST_MAP_WRITE))
    {
        GST_WARNING_OBJECT(filter, "failed to import destination frame into rga");
        flow = GST_FLOW_ERROR;
        goto out;
    }

    usage = gst_rga_convert_build_usage(
        gst_rga_convert_get_active_method(rgaconvert));

    /* Validate the requested operation against the RGA hardware limits. */
    status = imcheck(src.buffer, dst.buffer, empty_rect, empty_rect, usage);
    if (status != IM_STATUS_NOERROR)
    {
        GST_WARNING_OBJECT(filter, "rga check failed: %s", imStrError(status));
        flow = GST_FLOW_ERROR;
        goto out;
    }

    /* Scale + colour-convert (+ optional rotate/flip) in a single blit. */
    status = improcess(src.buffer, dst.buffer, empty_pat,
                       empty_rect, empty_rect, empty_rect, usage);
    if (status != IM_STATUS_SUCCESS)
    {
        GST_WARNING_OBJECT(filter, "rga process failed: %s", imStrError(status));
        flow = GST_FLOW_ERROR;
    }

out:
    gst_rga_frame_release(&dst, outframe);
    gst_rga_frame_release(&src, inframe);
    return flow;
}

static gboolean
plugin_init(GstPlugin *plugin)
{
    /* FIXME Remember to set the rank if it's an element that is meant
     to be autoplugged by decodebin. */
    return gst_element_register(plugin, "rgaconvert", GST_RANK_NONE,
                                GST_TYPE_RGA_CONVERT);
}


#ifndef VERSION
#define VERSION "1.0.0"
#endif
#ifndef PACKAGE
#define PACKAGE "rga_convert"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "rga_convert"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/higithubhi/gstreamer-rgaconvert.git"                            
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,
                  GST_VERSION_MINOR,
                  rgaconvert,
                  "video size colorspace convert for rockchip rga hardware",
                  plugin_init, VERSION, "GPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)
