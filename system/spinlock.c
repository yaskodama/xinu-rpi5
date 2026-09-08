// system/spinlock.c — 対称 SMP のスピンロック（排他版とパン屋版）。
//
// 排他版が非キャッシュ可能領域で動くかは実装依存なので、両方持って実機で
// 選べるようにしてある。詳細は include/spinlock.h の冒頭。

#include "spinlock.h"

#ifndef SPIN_BAKERY
#define SPIN_EXCLUSIVE 1
#endif

static inline void dmb(void)  { __asm__ volatile("dmb sy" ::: "memory"); }
static inline void dsb_(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void relax(void){ __asm__ volatile("yield" ::: "memory"); }

void spin_init(struct spinlock *l, const char *name)
{
    l->next_ticket = 0;
    l->now_serving = 0;
    for (int i = 0; i < SMP_NCORES; i++) { l->choosing[i] = 0; l->number[i] = 0; }
    l->acquires = 0;
    l->spins    = 0;
    l->name     = name;
    dsb_();
}

#ifdef SPIN_EXCLUSIVE

/* チケットロック: 自分の番号を原子的に取り、now_serving が追いつくまで待つ。
 * FIFO なので飢餓が無く、4 コアの公平性という点でも対称 SMP に向く。 */
static inline unsigned int fetch_add(volatile unsigned int *p, unsigned int v)
{
    unsigned int old, nw;
    unsigned int fail;
    do {
        __asm__ volatile("ldaxr %w0, [%2]\n\t"
                         "add   %w1, %w0, %w3\n\t"
                         "stxr  %w4, %w1, [%2]"
                         : "=&r"(old), "=&r"(nw), "+r"(p), "+r"(v), "=&r"(fail)
                         :: "memory");
    } while (fail);
    return old;
}

void spin_lock(struct spinlock *l)
{
    unsigned int me = fetch_add(&l->next_ticket, 1);
    unsigned long s = 0;
    while (l->now_serving != me) { relax(); s++; }
    dmb();
    l->acquires++;
    l->spins += s;
}

void spin_unlock(struct spinlock *l)
{
    dmb();
    l->now_serving = l->now_serving + 1;
    dsb_();
    __asm__ volatile("sev" ::: "memory");
}

const char *spin_impl_name(void) { return "ticket(ldaxr/stxr)"; }

#else  /* SPIN_BAKERY — 原子命令を一切使わない */

void spin_lock(struct spinlock *l)
{
    int me = smp_core_id(), i;
    unsigned long s = 0;
    l->choosing[me] = 1; dsb_();
    unsigned int max = 0;
    for (i = 0; i < SMP_NCORES; i++) if (l->number[i] > max) max = l->number[i];
    l->number[me] = max + 1; dsb_();
    l->choosing[me] = 0; dsb_();
    for (i = 0; i < SMP_NCORES; i++) {
        if (i == me) continue;
        while (l->choosing[i]) { relax(); s++; }
        while (l->number[i] != 0 &&
               (l->number[i] < l->number[me] ||
                (l->number[i] == l->number[me] && i < me))) { relax(); s++; }
    }
    dmb();
    l->acquires++;
    l->spins += s;
}

void spin_unlock(struct spinlock *l)
{
    dmb();
    l->number[smp_core_id()] = 0;
    dsb_();
}

const char *spin_impl_name(void) { return "bakery(no atomics)"; }

#endif

/* ---- 自己診断 ------------------------------------------------------ */
static struct spinlock  st_lock;
static volatile long    st_counter;
static volatile int     st_per_core;

static long st_worker(long lo, long hi, int core)
{
    (void)lo; (void)hi; (void)core;
    for (int i = 0; i < st_per_core; i++) {
        spin_lock(&st_lock);
        st_counter = st_counter + 1;      /* 保護されていなければ数が合わない */
        spin_unlock(&st_lock);
    }
    return 0;
}

long spin_selftest(int per_core)
{
    extern long smp_parallel_sum(long (*)(long, long, int), long, int);
    extern int  smp_cores_online(void);
    if (per_core < 1) per_core = 1000;
    spin_init(&st_lock, "selftest");
    st_counter  = 0;
    st_per_core = per_core;
    /* ワーカ郵便箱で 4 コアに同じ仕事を配り、ロックだけを試す。
     * （スケジューラを使わずロック単体を切り離して検証する） */
    int nc = smp_cores_online();
    (void)smp_parallel_sum(st_worker, (long)nc, nc);
    return st_counter;     /* 期待値 = per_core * nc */
}
