// kernel/memory.c — first-fit allocator (Xinu getmem/freemem style).

#include "memory.h"
#include "critical.h"

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

    /* The free list is shared with anything that allocates from an IRQ
     * handler or gets preempted mid-walk, so the whole walk is a critical
     * section.  proc_create() calls us with interrupts on. */
    unsigned long daif = irq_save();

    struct memblk *prev = &memlist_head;
    struct memblk *curr = memlist_head.mnext;
    void *result = 0;

    while (curr != 0) {
        /* A leftover smaller than a header cannot be described as a free
         * block: writing `struct memblk` at curr+nbytes would run past the
         * end of curr and clobber the successor.  Take the whole block
         * instead of splitting. */
        if (curr->mlength < nbytes + sizeof(struct memblk)) {
            if (curr->mlength >= nbytes) {
                /* Exact fit (or an unsplittable splinter): unlink whole. */
                prev->mnext = curr->mnext;
                memlist_head.mlength -= curr->mlength;
                result = (void *)curr;
                break;
            }
        } else {
            /* Split: keep the tail as a new free block. */
            struct memblk *leftover =
                (struct memblk *)((unsigned char *)curr + nbytes);
            leftover->mnext   = curr->mnext;
            leftover->mlength = curr->mlength - nbytes;
            prev->mnext = leftover;
            memlist_head.mlength -= nbytes;
            result = (void *)curr;
            break;
        }
        prev = curr;
        curr = curr->mnext;
    }

    irq_restore(daif);
    return result;  /* 0 = no fit */
}

int freemem(void *blk, unsigned long nbytes)
{
    if (blk == 0 || nbytes == 0) return -1;
    nbytes = ROUNDMB(nbytes);

    unsigned long start = (unsigned long)blk;
    unsigned long end   = start + nbytes;
    if (start < heap_lo || end > heap_hi) return -1;

    unsigned long daif = irq_save();

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
    if (prev_end > start)                        { irq_restore(daif); return -1; }
    if (curr != 0 && end > (unsigned long)curr)  { irq_restore(daif); return -1; }

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
    irq_restore(daif);
    return 0;
}

unsigned long mem_free_bytes(void)  { return memlist_head.mlength; }
unsigned long mem_total_bytes(void) { return heap_total; }

unsigned long mem_largest_block(void)
{
    unsigned long daif = irq_save();
    unsigned long max = 0;
    struct memblk *curr = memlist_head.mnext;
    while (curr != 0) {
        if (curr->mlength > max) max = curr->mlength;
        curr = curr->mnext;
    }
    irq_restore(daif);
    return max;
}

int mem_free_block_count(void)
{
    unsigned long daif = irq_save();
    int n = 0;
    struct memblk *curr = memlist_head.mnext;
    while (curr != 0) { n++; curr = curr->mnext; }
    irq_restore(daif);
    return n;
}

/* ===================================================================
 * Bare-metal libc-ish helpers.
 *
 * GCC silently lowers things like `= {0}` struct-initialisers and
 * pass-by-value struct returns into memset / memcpy calls — and we
 * don't link a standard library.  Providing these tiny versions here
 * keeps the linker quiet without bringing in newlib.  Names are
 * the standard ones so the compiler's implicit calls resolve.
 * =================================================================== */
void *memset(void *dst, int c, unsigned long n)
{
    unsigned char  v = (unsigned char)c;
    unsigned char *p = (unsigned char *)dst;
    for (unsigned long i = 0; i < n; i++) p[i] = v;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
