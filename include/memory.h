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

/* Round up / truncate to the memory-block granularity.
 *
 * MUST be >= sizeof(struct memblk) (16 on AArch64).  A free region smaller
 * than one header cannot be represented: freemem() would write mnext+mlength
 * (16 bytes) into an 8-byte hole and clobber the neighbouring block, and
 * getmem()'s split would write the leftover header past the end of the block
 * it is handing out.  With 16 every block size is a multiple of 16, so every
 * split remainder is 0 or >= 16 and both cases vanish.
 *
 * 16 is also what AArch64 needs: SP must be 16-byte aligned, and process
 * stacks come straight out of getmem() (system/proc.c).
 * Classic Xinu names; memory.c uses them in mem_init/getmem/freemem. */
#define MEM_ALIGN  16UL
#define ROUNDMB(x) ((unsigned long)(((unsigned long)(x) + (MEM_ALIGN-1UL)) & ~(MEM_ALIGN-1UL)))
#define TRUNCMB(x) ((unsigned long)( (unsigned long)(x)                    & ~(MEM_ALIGN-1UL)))

void mem_init(unsigned long heap_start, unsigned long heap_end);
void *getmem(unsigned long nbytes);
int   freemem(void *block, unsigned long nbytes);   /* 0 ok, -1 error */
unsigned long mem_free_bytes(void);
unsigned long mem_total_bytes(void);
unsigned long mem_largest_block(void);
int           mem_free_block_count(void);

extern unsigned char _end[];

#endif /* XINU_RPI5_MEMORY_H */
