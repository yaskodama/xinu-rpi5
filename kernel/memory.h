// kernel/memory.h — first-fit kernel heap allocator.
//
// Modelled on classic Embedded Xinu (system/getmem.c, system/freemem.c):
//   - Single free list of `struct memblk { mnext, mlength }` headers
//     that live IN-BAND at the start of each free region.
//   - getmem() walks the list first-fit, splits a leftover tail if
//     the block is larger than requested, and returns the front.
//   - freemem() inserts in address-sorted order with coalescing on
//     both the predecessor and successor sides.
//
// We don't have an MMU yet (phase M0), so this is the flat physical
// heap that lives between `_end` (top of static image) and HEAP_END
// (compile-time constant; see Makefile -DHEAP_END=...).

#ifndef XINU_RPI5_MEMORY_H
#define XINU_RPI5_MEMORY_H

#define MEM_ALIGN  16UL                              /* 16-byte alignment */
#define ROUNDMB(x) (((unsigned long)(x) + (MEM_ALIGN - 1)) & ~(MEM_ALIGN - 1))
#define TRUNCMB(x) ((unsigned long)(x) & ~(MEM_ALIGN - 1))

struct memblk {
    struct memblk *mnext;
    unsigned long  mlength;
};

void           mem_init(unsigned long heap_start, unsigned long heap_end);
void          *getmem(unsigned long nbytes);
int            freemem(void *blk, unsigned long nbytes);

unsigned long  mem_free_bytes(void);
unsigned long  mem_total_bytes(void);
unsigned long  mem_largest_block(void);
int            mem_free_block_count(void);

#endif /* XINU_RPI5_MEMORY_H */
