// system/smpsched.c — 対称 SMP スケジューラ（比較実験用の第2方式）。
// 設計と、意図的に対称化していない範囲は include/smpsched.h を見ること。

#include "proc.h"
#include "smp.h"
#include "smpsched.h"
#include "spinlock.h"
#include "critical.h"

#ifdef SMP_SYMMETRIC

int             smpsched_curr[SMP_NCORES];
struct spinlock proc_sched_lock;

/* proc.c 側が提供する（どちらもロックを保持した状態で呼ぶこと） */
extern struct procent *proc_ready_pop_locked(void);
extern void            proc_ready_push_locked(struct procent *p);

static unsigned long now_us(void)
{
    unsigned long ct, hz;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(ct));
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz ? (ct * 1000000UL) / hz : 0;
}

void smpsched_init(void)
{
    spin_init(&proc_sched_lock, "sched");
    smpsched_curr[0] = NULLPROC;
    for (int c = 1; c < SMP_NCORES; c++) {
        int pid = SMPIDLE_PID(c);
        struct procent *p = &proctab[pid];
        p->state   = PR_CURR;     /* 常に「そのコアで走っている」= 割り当て対象外 */
        p->prio    = 0;
        p->stkbase = 0;           /* 起動時のスタックをそのまま使う（NULLPROC と同じ手） */
        p->stklen  = 0;
        p->sp      = 0;           /* 最初の ctxsw OUT が本物を書く */
        p->next    = 0;
        p->name[0] = 'i'; p->name[1] = 'd'; p->name[2] = 'l';
        p->name[3] = 'e'; p->name[4] = '0' + c; p->name[5] = 0;
        smpsched_curr[c] = pid;
    }
}

/* 新規プロセスの入口。ctxsw.S の proc_entry_trampoline_smp から呼ばれ、
 * 切り替え元が握ったままのスケジューラ・ロックをここで離す。 */
void smpsched_after_switch(void)
{
    spin_unlock(&proc_sched_lock);
}

/* 核1〜3 の本体。ready キューから取っては走らせ、戻ってきたらまた取る。 */
void smpsched_core_loop(int core)
{
    int me = SMPIDLE_PID(core);
    struct procent *idle = &proctab[me];
    smpsched_curr[core] = me;
    extern int smp_worker_poll_once(int core);
    for (;;) {
        /* ① 郵便箱（従来方式）に仕事が来ていれば、それを片づける。
           同一カーネルで両方式を比較するために残してある。 */
        if (smp_worker_poll_once(core)) continue;
        /* ② 共有 ready キュー（対称方式） */
        spin_lock(&proc_sched_lock);
        struct procent *newp = proc_ready_pop_locked();
        if (newp == 0) {
            spin_unlock(&proc_sched_lock);
            __asm__ volatile("wfe" ::: "memory");
            continue;
        }
        newp->state = PR_CURR;
        smpsched_curr[core] = (int)(newp - proctab);
        ctxsw(&idle->sp, newp->sp);
        /* そのプロセスが exit / block してここへ戻る。ロックは相手が握ったまま
           渡してくるので、こちらで離す（xv6 と同じ受け渡し方式）。 */
        spin_unlock(&proc_sched_lock);
        smpsched_curr[core] = me;
    }
}

/* ================= 比較実験: 対称スケジューラ上の N-Queens ================ */

#define SM_MAXTASK 10
#define SM_STK     8192
static unsigned char sm_stk[SM_MAXTASK][SM_STK] __attribute__((aligned(16)));

static int             sm_n;
static int             sm_lo[NPROC], sm_hi[NPROC];
static volatile long   sm_res[NPROC];
static volatile int    sm_done;
static volatile int    sm_ran[SMP_NCORES];

static long nq(unsigned cols, unsigned d1, unsigned d2, unsigned all)
{
    if (cols == all) return 1;
    long count = 0;
    unsigned avail = ~(cols | d1 | d2) & all;
    while (avail) {
        unsigned bit = avail & (unsigned)(-(long)avail);
        avail -= bit;
        count += nq(cols | bit, (d1 | bit) << 1, (d2 | bit) >> 1, all);
    }
    return count;
}

static void sm_task(void)
{
    int pid  = currpid;                 /* コアごとの現在プロセス */
    int core = smp_core_id();
    unsigned all = (sm_n >= 32) ? 0xFFFFFFFFu : ((1u << sm_n) - 1u);
    long t = 0;
    for (int c = sm_lo[pid]; c < sm_hi[pid]; c++) {
        unsigned bit = 1u << c;
        t += nq(bit, bit << 1, bit >> 1, all);
    }
    sm_res[pid] = t;
    spin_lock(&proc_sched_lock);
    sm_ran[core]++;
    sm_done++;
    spin_unlock(&proc_sched_lock);
    proc_exit();                        /* 戻らない */
}

long smpsched_nqueens(int n, int ntasks, unsigned long *ms, int ran[SMP_NCORES])
{
    if (n < 1 || n > 16) n = 13;
    if (ntasks < 1) ntasks = SMP_NCORES;
    if (ntasks > SM_MAXTASK) ntasks = SM_MAXTASK;
    if (ntasks > n) ntasks = n;

    sm_n = n;
    sm_done = 0;
    for (int i = 0; i < SMP_NCORES; i++) sm_ran[i] = 0;

    int pids[SM_MAXTASK];
    /* ★ 先に「ready にせず」全部作り、範囲を入れてから投入する。
       生成と同時に ready にすると、範囲が未設定のまま他コアに拾われる。 */
    for (int t = 0; t < ntasks; t++) {
        int pid = proc_create_static_susp(sm_task, sm_stk[t], SM_STK, "nqtask");
        pids[t] = pid;
        if (pid < 0) { ntasks = t; break; }
        sm_lo[pid]  = (int)(((long)t * n) / ntasks);
        sm_hi[pid]  = (int)(((long)(t + 1) * n) / ntasks);
        sm_res[pid] = 0;
    }
    for (int t = 0; t < ntasks; t++) if (pids[t] >= 0) proc_ready(pids[t]);

    unsigned long t0 = now_us();
    /* 核0 も対等に参加する ―― 取れる仕事があれば自分で走らせる。 */
    while (sm_done < ntasks) proc_resched();
    unsigned long t1 = now_us();

    long total = 0;
    for (int t = 0; t < ntasks; t++) if (pids[t] >= 0) total += sm_res[pids[t]];
    if (ms)  *ms = (t1 - t0) / 1000UL;
    if (ran) for (int i = 0; i < SMP_NCORES; i++) ran[i] = sm_ran[i];
    return total;
}

#endif /* SMP_SYMMETRIC */
