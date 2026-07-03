# rgaconvert zero-copy dma-buf — design

**Date:** 2026-06-05
**Status:** Approved (design)
**Element:** `rgaconvert` (`plugins/gstrgaconvert.c`, `plugins/gstrgaconvert.h`)

## Goal

Make `rgaconvert` perform its NV12→BGRx (and other) conversions with no
per-frame CPU copy or per-frame MMU-mapping overhead, by importing the input
frame's dma-buf into RGA zero-copy and writing RGA's output directly into a
self-allocated dma-buf that the consumer maps.

Driving pipeline:

```
v4l2src (AHD camera, NV12, io-mode=dmabuf) \
    ! video/x-raw,format=NV12,width=W,height=H \
    ! rgaconvert \
    ! video/x-raw,format=BGRx,width=W,height=H \
    ! appsink
```

`appsink` is a **CPU consumer** — the application maps the output buffer and
reads BGRx. Zero-copy here therefore means:

1. Import the camera's NV12 dma-buf into RGA via `importbuffer_fd` (no copy).
2. Have RGA write BGRx straight into a dma-buf that the app then `mmap`s — no
   staging copy and no per-frame `importbuffer_virtualaddr` MMU setup.

## Target environment (verified)

- SoC: Rockchip RK3588, kernel 6.1.141.
- GStreamer 1.24.2; `gstreamer-allocators-1.0` 1.24.2 (provides
  `gst_dmabuf_allocator_new` / `gst_dmabuf_allocator_alloc`).
- dma-heap devices present: `/dev/dma_heap/system`, `system-uncached`, `cma`.
- libdrm 2.4.125 present but **not used** (dma-heap path needs no libdrm).
- librga `im2d` API in use: `importbuffer_fd(int fd, im_handle_param_t *param)`,
  `importbuffer_virtualaddr`, `wrapbuffer_handle`, `improcess`, `imcheck`.

## Scope decisions (settled during brainstorming)

- **Full zero-copy negotiation** at the *allocation layer*, not the caps layer.
- Caps stay plain `video/x-raw` *for the v4l2src→appsink path*. A dma-buf-backed
  `GstMemory` is still CPU-mappable, so plain caps + dma-buf memory is the
  correct pattern for that CPU consumer.

  **Update (2026-06-06):** both pads now *also* advertise the
  `video/x-raw(memory:DMABuf)` caps feature, negotiated independently per pad
  (see `gst_rga_convert_transform_caps`). This is required to link decoders that
  signal dma-buf at the caps layer (`mppvideodec`, `mppjpegdec`) — they
  otherwise fail with `rgaconvert can't handle caps video/x-raw(memory:DMABuf)`
  — and lets `rgaconvert` hand its dma-buf output zero-copy to a dma-buf consumer
  (e.g. `kmssink`). Plain `video/x-raw` is still offered, so the appsink CPU
  path is unchanged.
- **Output buffers are self-allocated** dma-bufs (appsink offers no dma-buf
  pool), with graceful fallback to the existing CPU path if the heap can't be
  opened.
- **Uncached heap** for output (`system-uncached`, fallback `cma`): CPU reads
  are always coherent with RGA's DMA writes, so no `DMA_BUF_IOCTL_SYNC` code and
  no custom mappable memory is required. Trade-off accepted: uncached CPU reads
  are somewhat slower than cached.

## Components

### 1. dma-heap allocation helper

A small internal helper (no new external dependency; uses
`<linux/dma-heap.h>` kernel uapi):

- Open the heap device **once** (lazily, cached on the element instance or the
  pool), preferring `/dev/dma_heap/system-uncached`, falling back to
  `/dev/dma_heap/cma`. If neither opens, report failure to the caller.
- `alloc(size)`: `ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data)` with
  `data.len = size`, `data.fd_flags = O_RDWR | O_CLOEXEC`, `data.heap_flags = 0`;
  return `data.fd`.

### 2. `GstRgaDmaBufPool` — `GstBufferPool` subclass

- `set_config`: parse the output caps into `GstVideoInfo`; apply the existing
  16-pixel stride alignment (`stride_align[i] = 15`) via `gst_video_info_align`;
  compute the aligned buffer size; support
  `GST_BUFFER_POOL_OPTION_VIDEO_META` and
  `GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT`.
- `alloc_buffer`: dma-heap-allocate an fd of the aligned size; wrap it with
  `gst_dmabuf_allocator_alloc(self->dmabuf_alloc, fd, size)` (the pool holds one
  `GstDmaBufAllocator` created with `gst_dmabuf_allocator_new()`); create the
  `GstBuffer`, append the memory, and attach a `GstVideoMeta` with the aligned
  strides/offsets via `gst_buffer_add_video_meta_full`.
- The pool keeps the heap device fd open for its lifetime and releases it on
  finalize.

### 3. `decide_allocation` (rewrite of the existing one)

- Compute the output `GstVideoInfo` from `outcaps` and the aligned size (reusing
  the current alignment logic).
- Construct a `GstRgaDmaBufPool`, configure it (caps, aligned size, min/max from
  the query or sane defaults), enable `VIDEO_META` + alignment, `set_config`.
- Install it as allocation pool index 0 so `GstBaseTransform` allocates output
  buffers from it.
- **Fallback:** if the heap cannot be opened / pool config fails, log a warning
  and keep the current behaviour (chain up to parent's `decide_allocation`,
  downstream pool + CPU import). The pipeline still works, just not zero-copy.

### 4. `propose_allocation` (new, sink side)

- Chain up to the parent implementation.
- Add the `GstVideoMeta` allocation meta
  (`gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL)`) so an
  upstream dma-buf producer attaches a `GstVideoMeta` with correct strides for
  `importbuffer_fd`.
- **Optional extension (documented, not enabled by default):** also propose our
  own dma-buf pool upstream so a `v4l2src io-mode=dmabuf-import` source can
  import into our buffers. Export-mode (`io-mode=dmabuf`) sources allocate their
  own buffers and need only the meta, which is the assumed default for the AHD
  camera. Decision: ship the meta-only version; leave the upstream-pool proposal
  as a documented follow-up.

### 5. Import path (`gst_rga_frame_import`)

- Keep the existing fd-import / CPU-map-fallback logic unchanged in behaviour:
  the dma-buf fd path is taken when the buffer has a single memory at offset 0
  (true for both our output pool buffers and single-fd NV12 camera buffers; NV12
  semi-planar chroma lives at an offset *within* the same fd and is handled by
  RGA via format + strides, not a separate `GstMemory`).
- **Add a `GST_DEBUG` log line** per import recording whether the zero-copy
  fd-import path or the CPU-map fallback was used, so zero-copy can be confirmed
  from logs.

### 6. Build / caps — no change

- Allocation uses kernel uapi (`<linux/dma-heap.h>`) and the already-present
  `gst_dmabuf_allocator` from `gstreamer-allocators-1.0`. No meson dependency
  changes.
- Pad templates and caps negotiation are unchanged (plain `video/x-raw`).

## Data flow (per frame)

1. Caps negotiated as today (e.g. NV12 in, BGRx out), plain `video/x-raw`.
2. `propose_allocation` (sink) requests `GstVideoMeta` → camera delivers a
   dma-buf NV12 buffer with stride metadata.
3. `decide_allocation` (src) installs the uncached dma-heap output pool with
   16-aligned strides.
4. `transform_frame`:
   - import `inframe` → `importbuffer_fd` (zero-copy) on the camera dma-buf.
   - import `outframe` → `importbuffer_fd` (zero-copy) on our pool dma-buf.
   - `imcheck` then `improcess` (`IM_SYNC`) convert/scale/rotate, writing into
     the output dma-buf.
   - release both RGA handles.
5. appsink receives the output `GstBuffer` (dma-buf memory), `mmap`s the
   uncached buffer, and reads BGRx coherently.

## Error handling

- Heap open failure → `decide_allocation` falls back to parent + CPU path
  (warning logged). Pipeline functional, non-zero-copy.
- Per-buffer dma-heap alloc failure → pool `alloc_buffer` returns an error →
  propagates as a flow error.
- fd-import failure → existing CPU-map fallback in `gst_rga_frame_import`.
- Unsupported pixel format → `set_info` rejects (existing behaviour).

## Testing strategy

This is a hardware-bound element (RK3588 RGA + dma-heap), so verification is
mostly integration; strict test-first is impractical for the RGA blit itself.

1. **dma-heap helper check (on target):** standalone test that opens the heap,
   allocates a buffer, `mmap`s it, writes and reads back — validates the helper
   independent of RGA.
2. **Negotiation/zero-copy confirmation:** run
   `GST_DEBUG=rgaconvert:5 gst-launch-1.0 v4l2src io-mode=dmabuf ! ... ! rgaconvert ! video/x-raw,format=BGRx ! fakesink`
   and assert the per-frame debug log shows zero-copy fd-import for **both** the
   src and dst frames and **no** CPU fallback.
3. **Output memory type:** a small appsink C test asserting the output buffer's
   memory satisfies `gst_is_dmabuf_memory()`.
4. **Pixel correctness:** feed a known NV12 frame and verify the BGRx output
   matches the expected conversion (sanity, not bit-exactness, given hardware
   conversion).
5. **Fallback path:** simulate heap-open failure (e.g. unreadable
   `/dev/dma_heap`) and confirm the element still converts via the CPU path.

## Out of scope

- ~~`memory:DMABuf` caps feature for opaque downstream (kmssink/GL)~~ —
  **implemented 2026-06-06** (both pads advertise the feature; see the scope
  update above).
- Cached heap + `DMA_BUF_IOCTL_SYNC` fast-read path — possible future
  optimisation if uncached CPU reads prove too slow.
- Proactively proposing an upstream dma-buf pool for `dmabuf-import` cameras —
  documented follow-up.
