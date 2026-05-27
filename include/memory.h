// include/memory.h — xinu-rpi5 first-fit allocator interface.

#ifndef XINU_RPI5_MEMORY_H
#define XINU_RPI5_MEMORY_H

struct memblk {
    struct memblk *mnext;
    unsigned long  mlength;
};

/* `struct memblock` is the xinu-raz free-list node.  The staged
 * xinu-raz network/ port pulls in extern/xinu-raz-include/thread.h,
 * whose `struct thrent` embeds a `struct memblock memlist` field by
 * value.  Because -I../include precedes -I../extern/xinu-raz-include,
 * that header resolves `<memory.h>` to *this* file rather than the
 * xinu-raz one, so the type must be complete here or those files fail
 * to compile.  Native code uses `struct memblk` (above), not this;
 * the layout matches extern/xinu-raz-include/memory.h. */
struct memblock {
    struct memblock *next;
    unsigned int     length;
};

/* Round up / truncate to the memory-block granularity (8 bytes — the
 * alignment of struct memblk's pointer/length fields on AArch64).
 * Classic Xinu names; memory.c uses them in mem_init/getmem/freemem. */
#define ROUNDMB(x) ((unsigned long)(((unsigned long)(x) + 7UL) & ~7UL))
#define TRUNCMB(x) ((unsigned long)( (unsigned long)(x)        & ~7UL))

void mem_init(unsigned long heap_start, unsigned long heap_end);
void *getmem(unsigned long nbytes);
int   freemem(void *block, unsigned long nbytes);   /* 0 ok, -1 error */
unsigned long mem_free_bytes(void);
unsigned long mem_total_bytes(void);
unsigned long mem_largest_block(void);
int           mem_free_block_count(void);

extern unsigned char _end[];

#endif /* XINU_RPI5_MEMORY_H */
