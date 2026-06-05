# rgaconvert zero-copy dma-buf Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `rgaconvert` import the input frame's dma-buf and write RGA's output into a self-allocated dma-buf, so NV12→BGRx conversion for an appsink consumer happens with no per-frame CPU copy or MMU-import overhead.

**Architecture:** Add a pure-C dma-heap allocation helper and a `GstBufferPool` subclass that hands out uncached dma-heap-backed, CPU-mappable dma-bufs. Wire the element's `decide_allocation` to install that pool for output (RGA writes into it via `importbuffer_fd`), and add `propose_allocation` to request `GstVideoMeta` from upstream so a dma-buf camera source is imported zero-copy. Negotiation stays at the allocation layer; caps remain plain `video/x-raw`.

**Tech Stack:** C, GStreamer 1.24 (`GstBaseTransform`/`GstVideoFilter`, `GstBufferPool`, `gst_dmabuf_allocator` from `gstreamer-allocators-1.0`), Linux dma-heap uapi (`<linux/dma-heap.h>`), Rockchip librga `im2d`. Build: meson + ninja (`ninja -C build`).

**Spec:** `docs/superpowers/specs/2026-06-05-rgaconvert-dmabuf-zerocopy-design.md`

**Branch:** `dmabuf-zerocopy` (already created; spec already committed there).

---

## File Structure

- Create `plugins/dmaheap.h` / `plugins/dmaheap.c` — pure-C dma-heap open/alloc helper. No GStreamer dependency; independently unit-testable.
- Create `plugins/gstrgadmabufpool.h` / `plugins/gstrgadmabufpool.c` — `GstRgaDmaBufPool`, a `GstBufferPool` subclass that allocates uncached dma-heap dma-bufs and wraps them with `gst_dmabuf_allocator_alloc`, attaching a `GstVideoMeta`.
- Modify `plugins/gstrgaconvert.c` — rewrite `gst_rga_convert_decide_allocation` to install the pool (with graceful fallback); add `gst_rga_convert_propose_allocation`; add a per-import zero-copy/CPU-fallback log line in `gst_rga_frame_import`.
- Modify `plugins/meson.build` — add the two new source pairs to `plugin_sources`.
- Create `tests/test-dmaheap.c`, `tests/test-rgapool.c`, `tests/test-zerocopy.c` — standalone verification programs (compiled directly with gcc; they are not wired into the plugin build).

---

## Task 1: dma-heap allocation helper

**Files:**
- Create: `plugins/dmaheap.h`
- Create: `plugins/dmaheap.c`
- Create: `tests/test-dmaheap.c`
- Modify: `plugins/meson.build`

- [ ] **Step 1: Write the failing test**

Create `tests/test-dmaheap.c`:

```c
#include "dmaheap.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

int
main (void)
{
    int heap = rga_dmaheap_open ();
    if (heap < 0) {
        fprintf (stderr, "FAIL: rga_dmaheap_open\n");
        return 1;
    }

    gsize len = 4096;
    int dfd = rga_dmaheap_alloc (heap, len);
    if (dfd < 0) {
        fprintf (stderr, "FAIL: rga_dmaheap_alloc\n");
        return 1;
    }

    void *p = mmap (NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
    if (p == MAP_FAILED) {
        fprintf (stderr, "FAIL: mmap\n");
        return 1;
    }

    memset (p, 0xAB, len);
    if (((unsigned char *) p)[0] != 0xAB
        || ((unsigned char *) p)[len - 1] != 0xAB) {
        fprintf (stderr, "FAIL: readback mismatch\n");
        return 1;
    }

    munmap (p, len);
    close (dfd);
    close (heap);
    printf ("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails (does not compile — header missing)**

Run:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
gcc tests/test-dmaheap.c plugins/dmaheap.c \
    -Iplugins $(pkg-config --cflags --libs glib-2.0) \
    -o /tmp/test-dmaheap
```
Expected: FAIL — `fatal error: dmaheap.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `plugins/dmaheap.h`:

```c
#ifndef _RGA_DMAHEAP_H_
#define _RGA_DMAHEAP_H_

#include <glib.h>

G_BEGIN_DECLS

/*
 * Open an uncached dma-heap device, preferring /dev/dma_heap/system-uncached
 * and falling back to /dev/dma_heap/cma. Returns an open fd (>= 0) that the
 * caller must close(), or -1 on failure (errno set).
 */
int rga_dmaheap_open (void);

/*
 * Allocate a dma-buf of 'len' bytes from the heap referenced by 'heap_fd'.
 * Returns a dma-buf fd (>= 0) the caller must close(), or -1 on failure
 * (errno set).
 */
int rga_dmaheap_alloc (int heap_fd, gsize len);

G_END_DECLS

#endif /* _RGA_DMAHEAP_H_ */
```

- [ ] **Step 4: Write the implementation**

Create `plugins/dmaheap.c`:

```c
#include "dmaheap.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const char *const rga_dmaheap_paths[] = {
    "/dev/dma_heap/system-uncached",
    "/dev/dma_heap/cma",
    NULL,
};

int
rga_dmaheap_open (void)
{
    for (int i = 0; rga_dmaheap_paths[i] != NULL; i++) {
        int fd = open (rga_dmaheap_paths[i], O_RDWR | O_CLOEXEC);
        if (fd >= 0)
            return fd;
    }
    return -1;
}

int
rga_dmaheap_alloc (int heap_fd, gsize len)
{
    struct dma_heap_allocation_data data = {
        .len = (guint64) len,
        .fd = 0,
        .fd_flags = O_RDWR | O_CLOEXEC,
        .heap_flags = 0,
    };

    if (heap_fd < 0 || len == 0) {
        errno = EINVAL;
        return -1;
    }

    if (ioctl (heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0)
        return -1;

    return (int) data.fd;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
gcc tests/test-dmaheap.c plugins/dmaheap.c \
    -Iplugins $(pkg-config --cflags --libs glib-2.0) \
    -o /tmp/test-dmaheap && /tmp/test-dmaheap
```
Expected: `PASS` (exit code 0).

- [ ] **Step 6: Add the helper to the plugin build**

In `plugins/meson.build`, change the `plugin_sources` list from:

```meson
plugin_sources = [
  'gstrgaconvert.c',
  'gstrgaconvert.h'
]
```

to:

```meson
plugin_sources = [
  'gstrgaconvert.c',
  'gstrgaconvert.h',
  'dmaheap.c',
  'dmaheap.h'
]
```

- [ ] **Step 7: Verify the plugin still builds**

Run:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
ninja -C build
```
Expected: build succeeds (the new source compiles; it is not yet referenced by the element).

- [ ] **Step 8: Commit**

```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
git add plugins/dmaheap.c plugins/dmaheap.h plugins/meson.build tests/test-dmaheap.c
git commit -m "feat(rgaconvert): add dma-heap allocation helper

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `GstRgaDmaBufPool` buffer pool

**Files:**
- Create: `plugins/gstrgadmabufpool.h`
- Create: `plugins/gstrgadmabufpool.c`
- Create: `tests/test-rgapool.c`
- Modify: `plugins/meson.build`

- [ ] **Step 1: Write the failing test**

Create `tests/test-rgapool.c`:

```c
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

    GstCaps *caps =
        gst_caps_from_string ("video/x-raw,format=BGRx,width=1280,height=720");
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
    if (gst_buffer_get_video_meta (buf) == NULL) {
        fprintf (stderr, "FAIL: buffer has no GstVideoMeta\n");
        return 1;
    }

    gst_buffer_unref (buf);
    gst_buffer_pool_set_active (pool, FALSE);
    gst_object_unref (pool);
    gst_caps_unref (caps);
    printf ("PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails (does not compile — header missing)**

Run:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
gcc tests/test-rgapool.c plugins/gstrgadmabufpool.c plugins/dmaheap.c \
    -Iplugins \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0) \
    -o /tmp/test-rgapool
```
Expected: FAIL — `fatal error: gstrgadmabufpool.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `plugins/gstrgadmabufpool.h`:

```c
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
```

- [ ] **Step 4: Write the implementation**

Create `plugins/gstrgadmabufpool.c`:

```c
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
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
gcc tests/test-rgapool.c plugins/gstrgadmabufpool.c plugins/dmaheap.c \
    -Iplugins \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0) \
    -o /tmp/test-rgapool && /tmp/test-rgapool
```
Expected: `PASS` (the acquired buffer is dma-buf memory and carries a `GstVideoMeta`).

- [ ] **Step 6: Add the pool to the plugin build**

In `plugins/meson.build`, extend `plugin_sources` to:

```meson
plugin_sources = [
  'gstrgaconvert.c',
  'gstrgaconvert.h',
  'dmaheap.c',
  'dmaheap.h',
  'gstrgadmabufpool.c',
  'gstrgadmabufpool.h'
]
```

- [ ] **Step 7: Verify the plugin builds**

Run:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
ninja -C build
```
Expected: build succeeds.

- [ ] **Step 8: Commit**

```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
git add plugins/gstrgadmabufpool.c plugins/gstrgadmabufpool.h plugins/meson.build tests/test-rgapool.c
git commit -m "feat(rgaconvert): add dma-heap-backed GstBufferPool

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Install the dma-buf pool in `decide_allocation`

**Files:**
- Modify: `plugins/gstrgaconvert.c` (add include; rewrite `gst_rga_convert_decide_allocation`)
- Create: `tests/test-zerocopy.c`

- [ ] **Step 1: Write the failing test**

Create `tests/test-zerocopy.c`. It builds `videotestsrc num-buffers=1 ! NV12 ! rgaconvert ! BGRx ! appsink` (no camera needed) and asserts the output buffer pulled from appsink is dma-buf memory — proving the output pool is in use:

```c
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
```

- [ ] **Step 2: Run the test to verify it fails**

Run (the plugin currently has no dma-buf pool, so the output buffer is plain system memory):
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
gcc tests/test-zerocopy.c -Iplugins \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0) \
    -o /tmp/test-zerocopy
GST_PLUGIN_PATH=$PWD/build/plugins /tmp/test-zerocopy
```
Expected: `FAIL: appsink buffer is not dma-buf memory`.

- [ ] **Step 3: Add the pool include to the element**

In `plugins/gstrgaconvert.c`, add this include next to the other local include near the top (it currently has `#include "gstrgaconvert.h"`):

```c
#include "gstrgaconvert.h"
#include "gstrgadmabufpool.h"
```

- [ ] **Step 4: Rewrite `gst_rga_convert_decide_allocation`**

Replace the entire existing `gst_rga_convert_decide_allocation` function body with the version below. It installs our dma-buf pool, honouring downstream's requested buffer counts, and falls back to the parent (CPU) path when no dma-heap is available:

```c
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
```

- [ ] **Step 5: Rebuild the plugin**

Run:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
ninja -C build
```
Expected: build succeeds.

- [ ] **Step 6: Run the test to verify it passes**

Run:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
GST_PLUGIN_PATH=$PWD/build/plugins /tmp/test-zerocopy
```
Expected: `PASS` (appsink's buffer is dma-buf memory — RGA wrote into our self-allocated dma-buf).

- [ ] **Step 7: Commit**

```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
git add plugins/gstrgaconvert.c tests/test-zerocopy.c
git commit -m "feat(rgaconvert): allocate dma-buf output via dma-heap pool

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Request `GstVideoMeta` from upstream via `propose_allocation`

**Files:**
- Modify: `plugins/gstrgaconvert.c` (add prototype, registration, and the function)

- [ ] **Step 1: Add the function prototype**

In `plugins/gstrgaconvert.c`, directly below the existing `gst_rga_convert_decide_allocation` prototype (near the other `static` prototypes), add:

```c
static gboolean gst_rga_convert_propose_allocation(GstBaseTransform *trans,
                                                   GstQuery *decide_query, GstQuery *query);
```

- [ ] **Step 2: Register the vfunc in `class_init`**

In `gst_rga_convert_class_init`, next to the existing
`base_transform_class->decide_allocation = ...` line, add:

```c
    base_transform_class->propose_allocation = GST_DEBUG_FUNCPTR(gst_rga_convert_propose_allocation);
```

- [ ] **Step 3: Implement the function**

Add this function to `plugins/gstrgaconvert.c` immediately after
`gst_rga_convert_decide_allocation`:

```c
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
```

- [ ] **Step 4: Rebuild the plugin**

Run:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
ninja -C build
```
Expected: build succeeds.

- [ ] **Step 5: Verify the meta is proposed on the sink pad**

The test from Task 3 still exercises this path (videotestsrc negotiates allocation against rgaconvert's sink). Re-run it to confirm no regression:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
GST_PLUGIN_PATH=$PWD/build/plugins /tmp/test-zerocopy
```
Expected: `PASS`.

Then confirm the proposed meta is visible in the allocation negotiation log:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
GST_PLUGIN_PATH=$PWD/build/plugins \
GST_DEBUG=GST_QUERY:5 /tmp/test-zerocopy 2>&1 | grep -i "GstVideoMeta\|meta-api" | head
```
Expected: at least one line referencing `GstVideoMeta` in an allocation query (the meta is being proposed/added).

- [ ] **Step 6: Commit**

```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
git add plugins/gstrgaconvert.c
git commit -m "feat(rgaconvert): propose GstVideoMeta to upstream for zero-copy import

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Log whether each frame import was zero-copy

**Files:**
- Modify: `plugins/gstrgaconvert.c` (`gst_rga_frame_import`)

- [ ] **Step 1: Add the log lines**

In `gst_rga_frame_import`, in the block that chooses between the fd import and
the CPU fallback, add a `GST_LOG` line to each branch. Change:

```c
    if (fd > 0)
    {
        rga_frame->handle = importbuffer_fd(fd, &param);
    }
    else
    {
        if (!gst_buffer_map(buf, &rga_frame->map_info, map_flags))
            return FALSE;
        rga_frame->mapped = TRUE;
        rga_frame->handle = importbuffer_virtualaddr(rga_frame->map_info.data, &param);
    }
```

to:

```c
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
```

- [ ] **Step 2: Rebuild the plugin**

Run:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
ninja -C build
```
Expected: build succeeds.

- [ ] **Step 3: Confirm the output frame is imported zero-copy**

Run the Task 3 pipeline with `rgaconvert` logging at LOG level. videotestsrc
produces a CPU (system-memory) input, so expect the *source* import to take the
CPU path and the *destination* (our dma-buf pool) import to be zero-copy:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
GST_PLUGIN_PATH=$PWD/build/plugins \
GST_DEBUG=rgaconvert:6 /tmp/test-zerocopy 2>&1 | grep "rga import:"
```
Expected: at least one `dma-buf fd ... (zero-copy)` line (the output frame). A
`CPU virtual address` line for the videotestsrc input is expected here and is
fine — the real camera (`io-mode=dmabuf`) input is verified in Task 6.

- [ ] **Step 4: Commit**

```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
git add plugins/gstrgaconvert.c
git commit -m "feat(rgaconvert): log zero-copy vs CPU import per frame

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: End-to-end verification on the camera pipeline

**Files:** none (verification only)

> This task validates the real target pipeline and the fallback path. It needs the AHD camera device. If the camera is not attached when executing, record that these steps are pending hardware and do not mark them complete.

- [ ] **Step 1: Confirm both input and output imports are zero-copy with the camera**

Replace `/dev/videoN` and the caps with the camera's actual node/format. Run:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
GST_PLUGIN_PATH=$PWD/build/plugins \
GST_DEBUG=rgaconvert:6 \
gst-launch-1.0 -v v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=30 ! \
    video/x-raw,format=NV12,width=1920,height=1080 ! \
    rgaconvert ! \
    video/x-raw,format=BGRx,width=1920,height=1080 ! \
    fakesink 2>&1 | grep "rga import:" | sort | uniq -c
```
Expected: `dma-buf fd ... (zero-copy)` lines for **both** the source and
destination imports, and **no** `CPU virtual address` lines.

- [ ] **Step 2: Confirm pixel output is correct**

Capture a few frames to a file and inspect (or feed your appsink consumer) to
confirm the BGRx output is visually correct:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
GST_PLUGIN_PATH=$PWD/build/plugins \
gst-launch-1.0 v4l2src device=/dev/video0 io-mode=dmabuf num-buffers=5 ! \
    video/x-raw,format=NV12,width=1920,height=1080 ! \
    rgaconvert ! video/x-raw,format=BGRx,width=1920,height=1080 ! \
    multifilesink location=/tmp/frame_%03d.bgrx
```
Expected: 5 files of size `1920*1080*4` bytes (stride-aligned size may be
larger); content is a correct color conversion of the camera frame.

- [ ] **Step 3: Confirm graceful fallback when no dma-heap is available**

Temporarily make the heap unavailable to exercise the fallback in
`decide_allocation` and the CPU import path, confirming the element still
converts:
```bash
cd /opt/flagman-store/dev/gstreamer-rgaconvert
sudo chmod 000 /dev/dma_heap/system-uncached /dev/dma_heap/cma
GST_PLUGIN_PATH=$PWD/build/plugins \
GST_DEBUG=rgaconvert:4 /tmp/test-zerocopy ; echo "exit=$?"
sudo chmod 660 /dev/dma_heap/system-uncached /dev/dma_heap/cma
```
Expected: a `no dma-heap available; falling back` warning is logged and the
program still runs to completion (the test asserts dma-buf output, so it will
print `FAIL` here — that is expected for this fallback check; the point is the
pipeline did not crash and logged the warning). Restore permissions as shown.

- [ ] **Step 4: Final commit (if any verification fixes were needed)**

If steps above required code changes, commit them. Otherwise record results in
the task notes. No commit is required for a clean verification pass.

---

## Notes for the implementer

- **Build:** always `ninja -C build` from the repo root. The build dir already
  exists and is configured; no `meson setup` is needed unless `meson.build`
  changes fail to reconfigure automatically (ninja triggers reconfigure on
  `meson.build` edits).
- **The standalone test programs** in `tests/` are compiled ad hoc with the
  `gcc` commands shown; they are intentionally not added to the meson build to
  keep the loop fast. Keep them in the repo as regression checks.
- **Working tree:** the in-progress edits to `plugins/gstrgaconvert.c` /
  `plugins/gstrgaconvert.h` present at the start are the baseline this plan
  builds on. The first commit of Task 1 only stages the new files plus
  `meson.build`; do not commit the pre-existing uncommitted `.c`/`.h` changes
  unless the user asks.
- **Cache coherency:** output uses an uncached dma-heap, so no `DMA_BUF_SYNC`
  is needed for the appsink CPU read. Do not add cached-heap support in this
  plan (it is an explicit out-of-scope future optimisation).
```
