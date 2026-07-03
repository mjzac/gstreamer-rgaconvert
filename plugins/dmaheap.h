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
