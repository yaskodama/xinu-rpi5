/* test/host/memtest.c — host-side regression tests for the first-fit
 * allocator in mem/memory.c.
 *
 * mem/memory.c is pure logic over a byte arena with no hardware
 * dependency, so it can be compiled and exercised natively.  Build+run:
 *
 *     make -C test/host run
 *
 * What this guards (both were live bugs, found by this harness):
 *
 *  1. getmem() splitting a block whose remainder is smaller than
 *     sizeof(struct memblk) wrote the leftover header PAST the end of the
 *     block it returned, corrupting the successor and splicing a bogus
 *     free node into the list.
 *  2. freemem() of a region smaller than sizeof(struct memblk) wrote the
 *     same 16-byte header into an 8-byte hole, clobbering the neighbour.
 *
 * Both are structurally impossible once MEM_ALIGN >= sizeof(struct memblk)
 * (include/memory.h), because then every size is a multiple of 16 and every
 * split remainder is 0 or >= 16.  The tests below assert that invariant
 * directly rather than the specific fix, so they stay meaningful if the
 * allocator is replaced.
 */

#include <stdio.h>
#include <stdlib.h>

/* memory.c defines memset/memcpy for the freestanding kernel; pull it in
 * before anything drags in <string.h> so the definitions don't collide. */
#include "../../mem/memory.c"

static unsigned char arena[65536] __attribute__((aligned(16)));

static int fails;

static void fail(const char *tag, const char *msg, unsigned long v)
{
    printf("FAIL [%s] %s (%lu)\n", tag, msg, v);
    fails++;
}

/* Walk the free list and assert every structural invariant we rely on. */
static void check_list(const char *tag)
{
    struct memblk *c = memlist_head.mnext;
    unsigned long sum = 0, prev = 0;
    int n = 0;

    while (c) {
        unsigned long a = (unsigned long)c;
        if (a < heap_lo || a + c->mlength > heap_hi)
            { fail(tag, "block outside arena", a); return; }
        if (a <= prev)
            { fail(tag, "free list not address-ordered", a); return; }
        if (c->mlength < sizeof(struct memblk))
            { fail(tag, "free block smaller than its header", c->mlength); return; }
        if (c->mlength % MEM_ALIGN)
            { fail(tag, "free block not a multiple of MEM_ALIGN", c->mlength); return; }
        if (a % MEM_ALIGN)
            { fail(tag, "free block misaligned", a); return; }
        prev = a; sum += c->mlength; c = c->mnext;
        if (++n > 100000) { fail(tag, "cycle in free list", 0); return; }
    }
    if (sum != memlist_head.mlength)
        fail(tag, "cached free total disagrees with walk", sum);
}

/* 1. Ask for a size that leaves a sub-header remainder. */
static void test_splinter(void)
{
    mem_init((unsigned long)arena, (unsigned long)arena + 4096);
    unsigned long avail = mem_free_bytes();

    /* Leave 8 bytes — smaller than the 16-byte header. */
    void *p = getmem(avail - 8);
    if (!p) { fail("splinter", "allocation refused", avail - 8); return; }

    check_list("splinter");
    /* The remainder must be absorbed, never described as a free block. */
    if (mem_free_block_count() != 0)
        fail("splinter", "unrepresentable remainder left on the list",
             (unsigned long)mem_free_block_count());
}

/* 2. A free of the smallest possible allocation must not overrun. */
static void test_min_free(void)
{
    mem_init((unsigned long)arena, (unsigned long)arena + 4096);
    void *a = getmem(1);
    void *b = getmem(1);
    if (!a || !b) { fail("min-free", "allocation refused", 0); return; }
    if (freemem(a, 1) != 0) fail("min-free", "freemem rejected a", 0);
    check_list("min-free");            /* a is now an isolated small hole */
    if (freemem(b, 1) != 0) fail("min-free", "freemem rejected b", 0);
    check_list("min-free-2");
    if (mem_free_bytes() != mem_total_bytes())
        fail("min-free", "leaked", mem_total_bytes() - mem_free_bytes());
}

/* 3. Ordinary split + coalesce round-trip. */
static void test_split_coalesce(void)
{
    mem_init((unsigned long)arena, (unsigned long)arena + 4096);
    void *p = getmem(1000);
    if (!p) { fail("split", "allocation refused", 1000); return; }
    check_list("split");
    if (mem_free_block_count() != 1)
        fail("split", "expected one free tail", (unsigned long)mem_free_block_count());
    if (freemem(p, 1000) != 0) fail("split", "freemem rejected", 0);
    check_list("split-free");
    if (mem_free_bytes() != mem_total_bytes())
        fail("split", "leaked", mem_total_bytes() - mem_free_bytes());
    if (mem_free_block_count() != 1)
        fail("split", "did not coalesce back to one block",
             (unsigned long)mem_free_block_count());
}

/* 4. Randomised churn — the case that actually caught bug 2. */
static void test_churn(void)
{
    mem_init((unsigned long)arena, (unsigned long)arena + sizeof arena);

    void *p[256];
    unsigned long sz[256];
    for (int i = 0; i < 256; i++) p[i] = 0;

    srand(12345);                       /* fixed seed: reproducible */
    for (int it = 0; it < 20000; it++) {
        int i = rand() % 256;
        if (p[i]) {
            if (freemem(p[i], sz[i]) != 0) { fail("churn", "freemem rejected", it); return; }
            p[i] = 0;
        } else {
            sz[i] = 1 + (unsigned long)(rand() % 300);
            p[i] = getmem(sz[i]);
            if (p[i]) memset(p[i], i & 0xFF, sz[i]);   /* touch every byte */
        }
        if (!(it % 97)) { check_list("churn"); if (fails) return; }
    }
    for (int i = 0; i < 256; i++) if (p[i]) freemem(p[i], sz[i]);

    check_list("churn-end");
    if (mem_free_bytes() != mem_total_bytes())
        fail("churn", "leaked after draining", mem_total_bytes() - mem_free_bytes());
    if (mem_free_block_count() != 1)
        fail("churn", "fully drained heap is fragmented",
             (unsigned long)mem_free_block_count());
}

/* 5. Bad frees must be rejected, not corrupt the list. */
static void test_bad_free(void)
{
    mem_init((unsigned long)arena, (unsigned long)arena + 4096);
    void *p = getmem(64);
    if (freemem(0, 64) == 0)                     fail("bad-free", "accepted NULL", 0);
    if (freemem(p, 0) == 0)                      fail("bad-free", "accepted zero size", 0);
    if (freemem((void *)(arena - 4096), 64) == 0) fail("bad-free", "accepted below-heap", 0);
    if (freemem((void *)(arena + 65536), 64) == 0) fail("bad-free", "accepted above-heap", 0);
    if (freemem(p, 64) != 0)                     fail("bad-free", "rejected valid free", 0);
    if (freemem(p, 64) == 0)                     fail("bad-free", "accepted double free", 0);
    check_list("bad-free");
}

int main(void)
{
    printf("MEM_ALIGN=%lu sizeof(struct memblk)=%lu\n",
           MEM_ALIGN, (unsigned long)sizeof(struct memblk));
    if (MEM_ALIGN < sizeof(struct memblk))
        fail("config", "MEM_ALIGN smaller than the free-list header", MEM_ALIGN);

    test_splinter();
    test_min_free();
    test_split_coalesce();
    test_churn();
    test_bad_free();

    if (fails) { printf("\n=== %d FAILURE(S) ===\n", fails); return 1; }
    printf("\n=== all memory tests passed ===\n");
    return 0;
}
