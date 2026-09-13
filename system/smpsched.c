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

/* ★ 対称スケジューラは「起動経路に入れない」。0 のあいだ二次コアは従来どおり
 * ワーカ郵便箱(smp_worker_loop)に居て、核0 だけがスケジューラを回す＝
 * 起動は SMP_SYM=0 の版と同じ道を通る。/smpmode?on=1 で初めて 1 になり、
 * 二次コアが共有 ready キューへ移る。
 * 実機で対称版が「Xinu の画面が出てから止まる」＝スケジューラ側の不具合で
 * 板ごと上がらなくなる、を二度とやらないための作り。 */
volatile int    smpsched_on = 0;
/* 0 = 全部 ready へ流す（従来）/ 1 = 核0 が 1 本を自分で持つ。/smpsched?mode= で選ぶ。 */
volatile int    smpsched_mode = 1;

void smpsched_enable(void)
{
    smpsched_on = 1;
    __asm__ volatile("dsb sy\n\tsev" ::: "memory");   /* 待っている二次コアを起こす */
}

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

/* 核0 用: ready に仕事があれば **1 本だけ**走らせて戻る。
 * 核0 は wm_run() の描画ループに居てスケジューラを一度も引かないので、
 * 対称モードでも核0 だけが働かない（実測: ran_per_core の先頭が常に 0）。
 * 描画ループから毎フレームこれを呼べば、核0 も共有 ready キューに参加する。
 * 走らせている間デスクトップは止まる —— 実験のための割り切り。
 * 現在の文脈（NULLPROC = 系のコンテキスト）は ready へ積まない。積むと
 * 二次コアに持って行かれて描画ループごと移動してしまう。 */
/* 計器: 核0(描画ループ)が何回引き取りに来て、何回仕事を取れたか。
 * 「核0が対称スケジューラに参加しているか」を数で見る。 */
static volatile long poll_calls, poll_hits;
long smpsched_poll_calls(void) { return poll_calls; }
long smpsched_poll_hits(void)  { return poll_hits; }

int smpsched_poll_once(void)
{
    if (!smpsched_on) return 0;
    poll_calls++;
    int core = smp_core_id();
    /* proc_resched と同じ作法で割り込みを止めてから文脈を切り替える。
       核0 は割り込みが生きているので、ctxsw の最中に横から入られないようにする。 */
    unsigned long d = irq_save();
    spin_lock(&proc_sched_lock);
    struct procent *newp = proc_ready_pop_locked();
    if (newp == 0) { spin_unlock(&proc_sched_lock); irq_restore(d); return 0; }
    int saved = smpsched_curr[core];
    struct procent *cur = &proctab[saved];
    newp->state = PR_CURR;
    smpsched_curr[core] = (int)(newp - proctab);
    ctxsw(&cur->sp, newp->sp);
    /* 相手が exit / block して戻ってきた。ロックは相手が握ったまま渡してくる。 */
    spin_unlock(&proc_sched_lock);
    smpsched_curr[core] = saved;
    irq_restore(d);
    poll_hits++;
    return 1;
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


/* ================= 汎用: 共有 ready キューで区間を分けて走らせる ==========
 * smp_parallel_sum（郵便箱・静的分割）の対称スケジューラ版。
 * AIPL のアクター・バッチ（device/video/avm.c の par_dispatch_range）を
 * どちらの方式でも配れるようにするために置く。これが無いと、対称モードでは
 * AIPL の並列実行が郵便箱経路に落ちて壊れる（実測: 82 秒・解が不一致）。
 *
 * fn(lo, hi, core) は [lo,hi) を処理して部分和を返す。core には**実際のコア番号**
 * を渡す（呼び先が per-core バッファを使うため）。
 * nunits を増やすほど動的に均される —— そこが静的分割との違い。 */
#define SP_MAXUNIT 4
#define SP_STK     8192
/* ★ 仮説: sp_task は sp_done++ したあとも **自分のスタックの上で** spin_unlock と
 *   proc_exit() を実行し続ける。駆動側は sp_done を見た瞬間に戻るので、
 *   次の呼び出しが同じ静的スタックへ新しい文脈を書き込む ―― まだ走っている
 *   タスクの足元を上書きすることになる。
 *   AIPL は毎秒数百回ここを通るので必ず踏み、N-Queens は HTTP 要求ごとに 1 回
 *   しか通らないので踏まない。観測（AIPL だけ壊れる）と一致する。
 *   対策: スタックを **世代** で回し、同じ領域を再利用するまでに数バッチ挟む。
 *   /spfix?on=0 で世代数を 1 に落とせば旧挙動に戻るので、同じ起動で A/B できる。 */
#define SP_GEN     4
static unsigned char sp_stk[SP_GEN][SP_MAXUNIT][SP_STK] __attribute__((aligned(16)));
static volatile int  sp_gen_on = 1;        /* 1=世代を回す(修正) 0=常に世代0(旧挙動) */
/* ★ 呼び手に持ち分を持たせない。計器がこう言っている:
 *     inline_us=4,150,086  wait_us=1,850  pp_inline=399
 *   生成した単位は 1.85 ms で終わっているのに、呼び手の持ち分だけが 4.15 秒。
 *   399 回の先取り × 1 ティック(10.4ms) = 4.15 秒 とぴたり一致する ――
 *   **先取り 1 回につき丸ごと 1 ティックを失っている**。
 *   呼び手が普通のプロセスとして共有 ready キューに載る対称モードでは、
 *   自分で計算させると必ずこれを踏む。全単位をプロセスにして待つだけにする。 */
static volatile int  sp_noinline = 1;      /* 1=呼び手は待つだけ(修正) 0=旧挙動 */
void smpsched_set_noinline(int on) { sp_noinline = on ? 1 : 0; }
int  smpsched_get_noinline(void)   { return sp_noinline; }
static volatile int  sp_gen    = 0;
static volatile long sp_hang   = 0;        /* 待ちが打ち切られた回数 */
static volatile long sp_maxwait_us = 0;    /* 待ちの最大値 */
static volatile long sp_reuse  = 0;        /* 再利用時にまだ PR_FREE でなかった回数 */
static volatile long sp_inline_us = 0;     /* 呼び手の持ち分に要した時間の最大値 */
static volatile long sp_wait_us   = 0;     /* 待ちループに要した時間の最大値 */
static volatile long sp_pp_inline  = 0;    /* 呼び手の持ち分の最中に起きた先取りの回数 */
static volatile long sp_poll_inline = 0;   /* 同じ区間で核0 が引き取った回数 */
long smpsched_inline_us(void)  { return sp_inline_us; }
long smpsched_wait_us(void)    { return sp_wait_us; }
long smpsched_pp_inline(void)  { return sp_pp_inline; }
long smpsched_poll_inline(void){ return sp_poll_inline; }
static volatile int  sp_busy_v = 0;        /* 診断用に外へ見せる */
void smpsched_set_genfix(int on) { sp_gen_on = on ? 1 : 0; }
int  smpsched_get_genfix(void)   { return sp_gen_on; }
long smpsched_hang(void)     { return sp_hang; }
long smpsched_maxwait(void)  { return sp_maxwait_us; }
long smpsched_reuse(void)    { return sp_reuse; }
int  smpsched_busy(void)     { return sp_busy_v; }
static smp_range_fn  sp_fn;
static int           sp_lo[NPROC], sp_hi[NPROC];
static int           sp_unit[NPROC];       /* 単位番号。配布関数の添字はこれを使う */
static volatile long sp_res[NPROC];
static volatile int  sp_done;
static volatile long sp_us;        /* 直近の並列区間に要した時間[us] */
static volatile long sp_calls;     /* 呼ばれた回数 */
static volatile long sp_units_tot; /* 配った単位の総数 */
static volatile int  sp_ran[SMP_NCORES];   /* コア別に走らせた単位数（この経路の分） */
long smpsched_ran(int core) { return (core >= 0 && core < SMP_NCORES) ? sp_ran[core] : 0; }

long smpsched_last_us(void)    { return sp_us; }
long smpsched_calls(void)      { return sp_calls; }
long smpsched_units_total(void){ return sp_units_tot; }

static void sp_task(void)
{
    int pid = currpid;
    /* 第3引数は「コア番号」ではなく「単位番号」。先取りで同じコアに別の単位が
       載っても、コアごとの控えに書き手が 2 つできないようにするため。 */
    sp_res[pid] = sp_fn ? sp_fn(sp_lo[pid], sp_hi[pid], sp_unit[pid]) : 0;
    spin_lock(&proc_sched_lock);
    sp_ran[smp_core_id()]++;
    sp_done++;
    spin_unlock(&proc_sched_lock);
    proc_exit();
}

long smpsched_parallel(smp_range_fn fn, long n, int nunits)
{
    /* 再入よけ。この関数は wm ループ（核0）から呼ばれ、待ちの proc_resched() で
     * ネットワーク・プロセスが走る。そこから何かがまた並列配布を要求しても
     * 大域（sp_res/sp_done…）を壊さないよう、入れ子は直列で処理する。 */
    static volatile int sp_busy;
    if (n <= 0) return 0;
    if (sp_busy) return fn(0, n, 0);          /* 入れ子は直列＝単位は 1 つ */
    if (nunits < 1) nunits = 1;
    if (nunits > SP_MAXUNIT) nunits = SP_MAXUNIT;
    if (nunits > n) nunits = (int)n;
    if (!smpsched_on || nunits == 1) return fn(0, n, 0);   /* 対称でなければ直列＝単位は 1 つ */

    sp_busy = 1; sp_busy_v = 1;
    sp_fn = fn;
    sp_done = 0;
    int g = sp_gen_on ? sp_gen : 0;            /* 使うスタック世代 */
    sp_gen = sp_gen_on ? ((sp_gen + 1) % SP_GEN) : 0;
    for (int i = 0; i < SMP_NCORES; i++) sp_ran[i] = 0;

    int pids[SP_MAXUNIT], nproc = 0;
    /* sp_noinline なら全単位をプロセスにする（呼び手は待つだけ）。 */
    int nspawn = sp_noinline ? nunits : (nunits - 1);
    for (int t = 0; t < nspawn; t++) {
        int pid = proc_create_static_susp(sp_task, sp_stk[g][t], SP_STK, "sptask");
        if (pid < 0) break;
        pids[nproc] = pid;
        sp_unit[pid] = t;
        sp_lo[pid] = (int)(((long)t * n) / nunits);
        sp_hi[pid] = (int)(((long)(t + 1) * n) / nunits);
        sp_res[pid] = 0;
        nproc++;
    }
    for (int t = 0; t < nproc; t++) proc_ready(pids[t]);

    unsigned long t0 = now_us();
    /* 呼び手の持ち分 = [nproc*n/nunits, n)。作れなかったぶんもここに入るので
       区間は必ずちょうど一度だけ覆われる。 */
    /* 呼び手の持ち分の単位番号は nproc（最後の単位）。ここも単位番号を渡す。 */
    extern unsigned long proc_dbg_ppfired(void);
    unsigned long pp0 = proc_dbg_ppfired();
    long pc0 = smpsched_poll_hits();
    unsigned long ti0 = now_us();
    /* 呼び手の持ち分 = [nproc*n/nunits, n)。全単位を出せていれば空区間になる。
       作れなかったぶんはここに入るので、区間は必ずちょうど一度だけ覆われる。 */
    long lo0 = (long)(((long)nproc * n) / nunits);
    long total = (lo0 < n) ? fn(lo0, n, nproc < SP_MAXUNIT ? nproc : SP_MAXUNIT - 1) : 0;
    { long d = (long)(now_us() - ti0); if (d > sp_inline_us) sp_inline_us = d; }
    { long d = (long)(proc_dbg_ppfired() - pp0); if (d > sp_pp_inline) sp_pp_inline = d; }
    { long d = smpsched_poll_hits() - pc0;       if (d > sp_poll_inline) sp_poll_inline = d; }
    unsigned long tw0 = now_us();
    sp_ran[smp_core_id()]++;                       /* 呼び手の持ち分も数える */
    /* 打ち切りつきの待ち。ハングしたときに sp_busy を握ったまま戻らないと、
       以後の呼び出しが全部直列に落ちる（実測: core_hits=493/0/0/0）。
       打ち切れば結果は揃わず検算に落ちる(ng++)が、**それが正しい見え方**である。 */
    while (sp_done < nproc) {
        if ((long)(now_us() - t0) > 500000L) { sp_hang++; break; }   /* 0.5 秒 */
        proc_resched();
    }
    { long w = (long)(now_us() - t0);  if (w > sp_maxwait_us) sp_maxwait_us = w; }
    { long w = (long)(now_us() - tw0); if (w > sp_wait_us) sp_wait_us = w; }
    sp_us = (long)(now_us() - t0);
    sp_calls++; sp_units_tot += nunits;

    for (int t = 0; t < nproc; t++) total += sp_res[pids[t]];
    /* 世代を一周して戻ってくるまでに、前の住人が PR_FREE になっているか。
       なっていなければ「まだ走っているスタックを上書きした」ことになる。 */
    for (int t = 0; t < nproc; t++)
        if (proctab[pids[t]].state != PR_FREE) sp_reuse++;
    sp_busy = 0; sp_busy_v = 0;
    return total;
}

/* ================= 比較実験: 対称スケジューラ上の N-Queens ================ */

#define SM_MAXTASK 6
#define SM_STK     32768
static unsigned char sm_stk[SM_MAXTASK][SM_STK] __attribute__((aligned(16)));
/* スタック使用量の実測。生成前に 0xA5 で埋め、終わってから下端から走査して
 * 「まだ 0xA5 のままの長さ」を引く＝実際に使った高さ。隣のタスクの領域まで
 * 食い込んでいれば、下端が壊れているので一目で分かる。 */
static int sm_stkuse[SM_MAXTASK];
static void sm_stk_fill(int t) { for (int i = 0; i < SM_STK; i++) sm_stk[t][i] = 0xA5; }
static int  sm_stk_used(int t) {
    int i = 0;
    while (i < SM_STK && sm_stk[t][i] == 0xA5) i++;
    return SM_STK - i;          /* 下端から連続する未使用ぶんを引いた値 */
}
int smpsched_stkuse(int i) { return (i >= 0 && i < SM_MAXTASK) ? sm_stkuse[i] : -1; }

static int             sm_n;
static int             sm_lo[NPROC], sm_hi[NPROC];
static volatile long   sm_res[NPROC];
static volatile int    sm_done;
static volatile int    sm_ran[SMP_NCORES];
/* 計器: なぜ核0 が 1 本も走らせないのかを数で見るために置く。
   sm_loop = 待ちループを回した回数、sched_pick = proc_resched が実際に
   ready から取れた回数（どちらもコア別）。 */
static volatile long   sm_loop[SMP_NCORES];
volatile long          sched_pick[SMP_NCORES];
static volatile int    sm_hits[NPROC];       /* 各タスクが何回走ったか */
static volatile int    sm_double;            /* 二度走ったタスクの数（0 が正しい） */
/* ★ 再入よけ。待ちループは proc_resched() でネットワーク・プロセスを走らせるので、
 * 実行が長引くと **同じ HTTP 要求の再送が処理され、この関数が二重に走る**。
 * 二つの実行が同じ大域（sm_res/sm_done…）を共有して合計が壊れる。
 * 実測: n=12(128ms) は正しく、n=13(600ms) だけ壊れた ―― 再送の時間に届くかどうかで
 * 切り替わる。値が毎回違うのも二重実行の徴候。 */
static volatile int    sm_busy;
/* 計器: タスク自身が使った pid と範囲。駆動側が設定した値と突き合わせる。
   食い違えば「タスクが別の pid の範囲を読んでいる」ことになる。 */
static volatile int    sm_seen_lo[NPROC], sm_seen_hi[NPROC], sm_seen_pid[NPROC];
/* 計器: 同じ入力でタスク自身に二度計算させ、食い違いを数える。食い違えば
 * 「切り替えで計算の途中状態が壊れている」ことが確定する。 */
static volatile int    sm_verify;
static volatile long   sm_res2[NPROC];
static volatile int    sm_mismatch;
static volatile int    sm_end_pid[NPROC];
void smpsched_set_verify(int v) { sm_verify = v ? 1 : 0; }
int  smpsched_mismatch(void)    { return sm_mismatch; }

/* 計器の読み出し（tcp_server から）。 */
long smpsched_loops(int core) { return (core >= 0 && core < SMP_NCORES) ? sm_loop[core] : 0; }
long smpsched_picks(int core) { return (core >= 0 && core < SMP_NCORES) ? sched_pick[core] : 0; }
long smpsched_double(void)    { return sm_double; }
/* 直近の走行の内訳（tcp_server が並べて出す）。i 番目のタスクの pid/範囲/結果。 */
static int  sm_np;
static int  sm_pidv[SM_MAXTASK];
int  smpsched_nslots(void) { return sm_np; }
int  smpsched_slot_pid(int i){ return (i>=0 && i<sm_np) ? sm_pidv[i] : -1; }
long smpsched_slot_res(int i){ int p=smpsched_slot_pid(i); return (p>=0)? sm_res[p] : -1; }
long smpsched_res2(int i)      { int p=smpsched_slot_pid(i); return (p>=0)? sm_res2[p] : -1; }
int  smpsched_slot_endpid(int i){ int p=smpsched_slot_pid(i); return (p>=0)? sm_end_pid[p&(NPROC-1)] : -1; }
int  smpsched_slot_lo(int i) { int p=smpsched_slot_pid(i); return (p>=0)? sm_lo[p] : -1; }
int  smpsched_slot_hi(int i) { int p=smpsched_slot_pid(i); return (p>=0)? sm_hi[p] : -1; }
int  smpsched_slot_seenlo(int i){ int p=smpsched_slot_pid(i); return (p>=0)? sm_seen_lo[p&(NPROC-1)] : -1; }
int  smpsched_slot_seenhi(int i){ int p=smpsched_slot_pid(i); return (p>=0)? sm_seen_hi[p&(NPROC-1)] : -1; }
static long sm_inline_res; static int sm_inline_lo, sm_inline_hi;
long smpsched_inline_res(void){ return sm_inline_res; }
int  smpsched_inline_lo(void) { return sm_inline_lo; }
int  smpsched_inline_hi(void) { return sm_inline_hi; }


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
    sm_seen_pid[pid & (NPROC-1)] = pid;         /* 計器: 自分が何番だと思ったか */
    sm_seen_lo[pid & (NPROC-1)]  = sm_lo[pid];  /*       どの範囲を読んだか     */
    sm_seen_hi[pid & (NPROC-1)]  = sm_hi[pid];
    unsigned all = (sm_n >= 32) ? 0xFFFFFFFFu : ((1u << sm_n) - 1u);
    long t = 0;
    for (int c = sm_lo[pid]; c < sm_hi[pid]; c++) {
        unsigned bit = 1u << c;
        t += nq(bit, bit << 1, bit >> 1, all);
    }
    sm_res[pid] = t;
    if (sm_verify) {                    /* 同じ範囲をもう一度、同じタスクの中で */
        long t2 = 0;
        for (int c = sm_lo[pid]; c < sm_hi[pid]; c++) {
            unsigned bit = 1u << c;
            t2 += nq(bit, bit << 1, bit >> 1, all);
        }
        sm_res2[pid] = t2;
        if (t2 != t) sm_mismatch++;
    }
    sm_end_pid[pid & (NPROC-1)] = pid;
    spin_lock(&proc_sched_lock);
    sm_ran[core]++;
    sm_done++;
    if (++sm_hits[pid] > 1) sm_double++;      /* 二度走ったら数える（解が狂う原因） */
    spin_unlock(&proc_sched_lock);
    proc_exit();                        /* 戻らない */
}

/* 範囲 [lo,hi) の列を数える（核0 が自分の持ち分を直接走らせるときに使う）。 */
static long nq_range(int n, int lo, int hi)
{
    unsigned all = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    long t = 0;
    for (int c = lo; c < hi; c++) {
        unsigned bit = 1u << c;
        t += nq(bit, bit << 1, bit >> 1, all);
    }
    return t;
}

long smpsched_nqueens(int n, int ntasks, unsigned long *ms, int ran[SMP_NCORES])
{
    if (sm_busy) { if (ms) *ms = 0; if (ran) for (int i=0;i<SMP_NCORES;i++) ran[i]=0; return -1; }
    sm_busy = 1;
    if (n < 1 || n > 16) n = 13;
    if (ntasks < 1) ntasks = SMP_NCORES;
    if (ntasks > SM_MAXTASK) ntasks = SM_MAXTASK;
    if (ntasks > n) ntasks = n;

    sm_n = n;
    sm_done = 0;
    sm_double = 0; sm_mismatch = 0;
    for (int i = 0; i < SMP_NCORES; i++) { sm_ran[i] = 0; sm_loop[i] = 0; sched_pick[i] = 0; }

    /* ndiv = 範囲の分母。**何があっても変えない**。
       前の版は生成に失敗したとき ntasks だけ縮めていたので、既に作った
       タスクの範囲（古い分母で計算済み）と食い違い、列が抜ける/重なる。
       これが切替前に解が狂った原因の第一候補。 */
    const int ndiv = ntasks;
    /* mode=1: 最後の 1 本は駆動側（この呼び出しを実行しているコア）が直接走らせる。
       走らせない物は作らない（作って PR_FREE で捨てる形はやめた）。 */
    const int want_proc = (smpsched_mode == 1 && ndiv > 1) ? ndiv - 1 : ndiv;

    int pids[SM_MAXTASK];
    int nproc = 0;
    /* 先に「ready にせず」作り、範囲を入れてから投入する。生成と同時に ready に
       すると、範囲が未設定のまま他コアに拾われる。 */
    for (int t = 0; t < want_proc; t++) {
        sm_stk_fill(t);
        int pid = proc_create_static_susp(sm_task, sm_stk[t], SM_STK, "nqtask");
        if (pid < 0) break;                 /* 作れなかったぶんは駆動側が引き受ける */
        pids[nproc] = pid;
        sm_lo[pid]  = (int)(((long)t * n) / ndiv);
        sm_hi[pid]  = (int)(((long)(t + 1) * n) / ndiv);
        sm_res[pid] = 0;
        sm_hits[pid] = 0;
        nproc++;
    }
    /* 駆動側の持ち分は [nproc, ndiv) ―― mode=1 の 1 本ぶんに加えて、
       生成できなかったぶんもここに入る。これで列は必ず全部ちょうど一度覆う。 */
    const int inline_lo = (int)(((long)nproc * n) / ndiv);
    const int inline_hi = n;
    const int have_inline = (inline_lo < inline_hi) ? 1 : 0;
    const int target = nproc + have_inline;

    sm_np = nproc;
    for (int t = 0; t < nproc; t++) { sm_pidv[t] = pids[t]; sm_seen_pid[pids[t]&(NPROC-1)] = -1;
                                      sm_seen_lo[pids[t]&(NPROC-1)] = -1; sm_seen_hi[pids[t]&(NPROC-1)] = -1; }
    for (int t = 0; t < nproc; t++) proc_ready(pids[t]);

    unsigned long t0 = now_us();
    long inline_res = 0;
    sm_inline_lo = inline_lo; sm_inline_hi = have_inline ? inline_hi : inline_lo;
    sm_inline_res = 0;
    if (have_inline) {
        inline_res = nq_range(n, inline_lo, inline_hi);
        sm_inline_res = inline_res;
        spin_lock(&proc_sched_lock);
        sm_ran[smp_core_id()]++; sm_done++;
        spin_unlock(&proc_sched_lock);
    }
    /* 残りは共有 ready キューから。駆動側もここで取りに行く。 */
    while (sm_done < target) { sm_loop[smp_core_id()]++; proc_resched(); }
    unsigned long t1 = now_us();

    for (int t = 0; t < nproc; t++) sm_stkuse[t] = sm_stk_used(t);
    long total = inline_res;
    for (int t = 0; t < nproc; t++) total += sm_res[pids[t]];
    if (ms)  *ms = (t1 - t0) / 1000UL;
    if (ran) for (int i = 0; i < SMP_NCORES; i++) ran[i] = sm_ran[i];
    sm_busy = 0;
    return total;
}

#endif /* SMP_SYMMETRIC */
