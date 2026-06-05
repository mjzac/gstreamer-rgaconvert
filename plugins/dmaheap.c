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
