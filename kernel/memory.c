// kernel/memory.c — first-fit allocator (Xinu getmem/freemem style).

#include "memory.h"

/* Dummy list head.  `mnext` points at the first real free block;
 * `mlength` shadows the sum of all free bytes so mem_free_bytes()
 * is O(1).  Real Xinu walks the list on every query — we trade a
 * cheap update for a faster `mem` command. */
static struct memblk memlist_head;

static unsigned long heap_lo;   /* inclusive */
static unsigned long heap_hi;   /* exclusive */
static unsigned long heap_total;

void mem_init(unsigned long heap_start, unsigned long heap_end)
{
    heap_start = ROUNDMB(heap_start);
    heap_end   = TRUNCMB(heap_end);

    heap_lo    = heap_start;
    heap_hi    = heap_end;
    heap_total = heap_end - heap_start;

    struct memblk *blk = (struct memblk *)heap_start;
    blk->mnext   = 0;
    blk->mlength = heap_total;

    memlist_head.mnext   = blk;
    memlist_head.mlength = heap_total;
}

void *getmem(unsigned long nbytes)
{
    if (nbytes == 0) return 0;
    nbytes = ROUNDMB(nbytes);

    struct memblk *prev = &memlist_head;
    struct memblk *curr = memlist_head.mnext;

    while (curr != 0) {
        if (curr->mlength == nbytes) {
            /* Exact fit: unlink whole block. */
            prev->mnext = curr->mnext;
            memlist_head.mlength -= nbytes;
            return (void *)curr;
        }
        if (curr->mlength > nbytes) {
            /* Split: keep the tail as a new free block. */
            struct memblk *leftover =
                (struct memblk *)((unsigned char *)curr + nbytes);
            leftover->mnext   = curr->mnext;
            leftover->mlength = curr->mlength - nbytes;
            prev->mnext = leftover;
            memlist_head.mlength -= nbytes;
            return (void *)curr;
        }
        prev = curr;
        curr = curr->mnext;
    }
    return 0;  /* no fit */
}

int freemem(void *blk, unsigned long nbytes)
{
    if (blk == 0 || nbytes == 0) return -1;
    nbytes = ROUNDMB(nbytes);

    unsigned long start = (unsigned long)blk;
    unsigned long end   = start + nbytes;
    if (start < heap_lo || end > heap_hi) return -1;

    /* Walk to the insertion point so prev < blk <= curr. */
    struct memblk *prev = &memlist_head;
    struct memblk *curr = memlist_head.mnext;
    while (curr != 0 && (unsigned long)curr < start) {
        prev = curr;
        curr = curr->mnext;
    }

    /* Overlap check (catches double-free / wrong-size). */
    unsigned long prev_end =
        (prev == &memlist_head) ? 0 : ((unsigned long)prev + prev->mlength);
    if (prev_end > start) return -1;
    if (curr != 0 && end > (unsigned long)curr) return -1;

    struct memblk *newblk = (struct memblk *)start;

    /* Coalesce with predecessor when adjacent. */
    if (prev != &memlist_head && prev_end == start) {
        prev->mlength += nbytes;
        newblk = prev;
    } else {
        newblk->mnext   = curr;
        newblk->mlength = nbytes;
        prev->mnext = newblk;
    }

    /* Coalesce with successor when adjacent. */
    if (curr != 0 &&
        ((unsigned long)newblk + newblk->mlength) == (unsigned long)curr) {
        newblk->mlength += curr->mlength;
        newblk->mnext = curr->mnext;
    }

    memlist_head.mlength += nbytes;
    return 0;
}

unsigned long mem_free_bytes(void)  { return memlist_head.mlength; }
unsigned long mem_total_bytes(void) { return heap_total; }

unsigned long mem_largest_block(void)
{
    unsigned long max = 0;
    struct memblk *curr = memlist_head.mnext;
    while (curr != 0) {
        if (curr->mlength > max) max = curr->mlength;
        curr = curr->mnext;
    }
    return max;
}

int mem_free_block_count(void)
{
    int n = 0;
    struct memblk *curr = memlist_head.mnext;
    while (curr != 0) { n++; curr = curr->mnext; }
    return n;
}
