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
