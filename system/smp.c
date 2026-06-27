// system/smp.c — worker-pool SMP bring-up + lock-free parallel dispatch.
// See include/smp.h for the architecture rationale.  Ported from xinu-rpi4
// (BCM2711 4×A72) to the Pi 5's BCM2712 4×Cortex-A76 — the secondary-core
// spin-table mailbox lives at the same low-RAM addresses (0xe0/0xe8/0xf0).

#include "smp.h"

/* ---- boot.S handoff (these live in boot.S's .data so they read as 0 from
 * image load, before core 0 clears .bss — a secondary spinning in the wfe
 * poll must never see a stale non-zero release address). ---- */
extern volatile unsigned long smp_release[SMP_NCORES];   /* entry addr per core */
extern volatile unsigned long smp_stacktop[SMP_NCORES];  /* initial SP per core */
extern void _smp_start(void);                            /* boot.S trampoline   */

/* Each secondary core's idle stack (no heap dependency at bring-up time). */
#define SMP_STACK_BYTES 16384
static unsigned char smp_stack[SMP_NCORES][SMP_STACK_BYTES] __attribute__((aligned(16)));

/* Online flags + the per-core job mailbox.  All cross-core, all volatile;
 * coherent without locks because the D-cache is off (every access hits RAM). */
static volatile int          smp_online[SMP_NCORES];
static volatile smp_range_fn smp_job_fn[SMP_NCORES];
static volatile long         smp_job_lo[SMP_NCORES];
static volatile long         smp_job_hi[SMP_NCORES];
static volatile long         smp_job_res[SMP_NCORES];
static volatile int          smp_job_seq[SMP_NCORES];    /* bumped to post a job */
static volatile int          smp_job_done[SMP_NCORES];   /* == seq when finished */

/* Bound on how long core 0 waits for a worker before taking the chunk over
 * itself.  ~2e9 spin iterations ≈ a few seconds — far longer than any real
 * chunk, so it only trips on a genuinely dead/never-started core. */
#define SMP_WAIT_LIMIT 2000000000UL

/* Bring-up wait: a released core announces itself in microseconds, so cap the
 * per-core online wait short (~tens of ms) — if a core does not respond in that
 * window it is treated as offline and boot proceeds (no multi-second stall). */
#define SMP_BRINGUP_WAIT 100000000UL

static inline void dsb_sev(void) { __asm__ volatile("dsb sy\n\tsev" ::: "memory"); }
static inline void dsb(void)     { __asm__ volatile("dsb sy" ::: "memory"); }

/* PSCI (Power State Coordination Interface).  The Pi 5 boots ARM Trusted
 * Firmware (BL31) at EL3 — confirmed by the "NOTICE: BL31:" line on the serial
 * console — which holds cores 1-3 and starts them only on a PSCI CPU_ON SMC.
 * The legacy 0xe0/0xe8/0xf0 spin-table mailbox (Pi 3/4 armstub) is NOT honoured
 * on the Pi 5, so we must use PSCI here. */
#ifdef RP1_ETH_BASE   /* Pi 5 only: BL31 present, SMC conduit (see DT psci node) */
#define PSCI_CPU_ON_AARCH64  0xC4000003UL

static long psci_cpu_on(unsigned long target_cpu, unsigned long entry,
                        unsigned long context_id)
{
    register long x0 __asm__("x0") = (long)PSCI_CPU_ON_AARCH64;
    register long x1 __asm__("x1") = (long)target_cpu;   /* MPIDR aff (= core id) */
    register long x2 __asm__("x2") = (long)entry;         /* phys entry point      */
    register long x3 __asm__("x3") = (long)context_id;    /* -> x0 of started core */
    __asm__ volatile("smc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "x4","x5","x6","x7","x8","x9","x10","x11","x12","x13",
                       "x14","x15","x16","x17","memory","cc");
    return x0;   /* 0 = SUCCESS, negative = PSCI error */
}
#endif /* RP1_ETH_BASE */

int smp_core_id(void)
{
    unsigned long m;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(m));
    /* DynamIQ (Pi 5 A76): MPIDR.MT=1, core id is in Aff1 (bits 15:8) — the DT
     * gives cpu@c reg = 0x100*c.  Cluster (Pi 4 A72): MT=0, core id in Aff0. */
    unsigned long aff = (m & (1UL << 24)) ? ((m >> 8) & 0xff) : (m & 0xff);
    return (int)(aff & 3);
}

/* The worker idle loop: wait (low-power) for a new job, run it, signal done. */
static void smp_worker_loop(int core)
{
    int last = smp_job_seq[core];
    for (;;) {
        while (smp_job_seq[core] == last) __asm__ volatile("wfe");
        last = smp_job_seq[core];
        smp_range_fn fn = smp_job_fn[core];
        long r = fn ? fn(smp_job_lo[core], smp_job_hi[core], core) : 0;
        smp_job_res[core] = r;
        dsb();
        smp_job_done[core] = last;       /* publish completion after the result */
        dsb_sev();                       /* wake core 0 out of its wait spin     */
    }
}

/* C entry for a freshly-started secondary core (called from boot.S at EL1 with
 * its stack already set).  Match core 0's MMU/cache config for fair timing,
 * install the shared exception vectors, announce online, then idle. */
void smp_secondary_entry(int core)
{
    extern void exception_init(void);       /* VBAR_EL1 -> shared vector table */
    extern void mmu_enable_secondary(void); /* MMU on, caches off (matches CPU0) */
    if (core < 0 || core >= SMP_NCORES) { for (;;) __asm__ volatile("wfe"); }
    exception_init();
    mmu_enable_secondary();
    smp_online[core] = 1;
    dsb_sev();                              /* tell core 0 we are up */
    smp_worker_loop(core);                  /* never returns */
}

void smp_init(void)
{
    smp_online[0] = 1;                       /* core 0 is obviously up */
    for (int c = 1; c < SMP_NCORES; c++) {
        smp_job_seq[c]  = 0;
        smp_job_done[c] = 0;
        smp_stacktop[c] = (unsigned long)(smp_stack[c] + SMP_STACK_BYTES);
    }
    dsb();

    /* Start each secondary via PSCI CPU_ON (the Pi 5 / BL31 mechanism — see
     * psci_cpu_on above).  The started core enters _smp_start (phys) with the
     * MMU off; boot.S sets its stack from smp_stacktop[] and calls into C.
     * smp_release[] is left set as a harmless fallback for the boot.S wfe-park
     * path (only taken if some firmware instead drops cores into _start). */
    for (int c = 1; c < SMP_NCORES; c++) {
        smp_release[c] = (unsigned long)&_smp_start;
        dsb();
#ifdef RP1_ETH_BASE
        /* Pi 5: target MPIDR carries the core id in Aff1 (cpu@c reg = 0x100*c). */
        long rc = psci_cpu_on((unsigned long)c << 8, (unsigned long)&_smp_start, 0);
        (void)rc;   /* rc<0 -> core stays offline; parallel jobs fall back to CPU0 */
#endif
    }

    /* Wait (bounded) for each to announce itself online.  A core that does not
     * respond in this window is treated as offline and boot proceeds. */
    for (int c = 1; c < SMP_NCORES; c++) {
        unsigned long spins = 0;
        while (!smp_online[c] && ++spins < SMP_BRINGUP_WAIT) __asm__ volatile("nop");
    }
}

int smp_cores_online(void)
{
    int n = 0;
    for (int c = 0; c < SMP_NCORES; c++) if (smp_online[c]) n++;
    return n;
}

long smp_parallel_sum(smp_range_fn fn, long n, int ncores)
{
    if (ncores < 1) ncores = 1;
    if (ncores > SMP_NCORES) ncores = SMP_NCORES;
    if (n < 0) n = 0;

    long chunk = n / ncores;
    long total = 0;

    /* Post chunks 1..ncores-1 to the worker cores (skip offline ones — those
     * chunks are computed by core 0 below). */
    for (int c = 1; c < ncores; c++) {
        long lo = (long)c * chunk;
        long hi = (c == ncores - 1) ? n : lo + chunk;
        if (!smp_online[c]) { total += fn(lo, hi, 0); continue; }
        smp_job_fn[c] = fn;
        smp_job_lo[c] = lo;
        smp_job_hi[c] = hi;
        dsb();
        smp_job_seq[c]++;        /* arm the job, then wake the worker */
        dsb_sev();
    }

    /* Core 0 runs chunk 0 inline while the workers run theirs. */
    total += fn(0, (ncores == 1) ? n : chunk, 0);

    /* Collect the workers, taking over any that did not finish in time. */
    for (int c = 1; c < ncores; c++) {
        if (!smp_online[c]) continue;          /* already done inline above */
        unsigned long spins = 0;
        while (smp_job_done[c] != smp_job_seq[c]) {
            if (++spins >= SMP_WAIT_LIMIT) {   /* worker stuck — do it here */
                long lo = (long)c * chunk;
                long hi = (c == ncores - 1) ? n : lo + chunk;
                smp_job_res[c] = fn(lo, hi, 0);
                break;
            }
            __asm__ volatile("nop");
        }
        total += smp_job_res[c];
    }
    return total;
}

/* ===================================================================
 *  Boot-time self-test / benchmark: count primes in [0,n) on 1 core,
 *  then on all online cores, and print wall-clock times + speedup to
 *  the serial console.  Lets us compare 1-core vs 4-core scaling even
 *  with no network up.  Also exposed over HTTP via /smp-bench.
 * =================================================================== */

extern void uart_puts(const char *s);
extern void uart_putc(char c);

/* Print an unsigned decimal to the UART. */
static void uart_dec(unsigned long v)
{
    char buf[24];
    int i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v && i < (int)sizeof buf) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) uart_putc(buf[i]);
}

/* Free-running counter in milliseconds (CNTPCT_EL0 / CNTFRQ_EL0 * 1000). */
static unsigned long smp_now_ms(void)
{
    unsigned long ct, hz;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(ct));
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(hz));
    if (!hz) return 0;
    return (ct * 1000UL) / hz;
}

/* Prime-count kernel (trial division) — pure integer compute, matches
 * smp_range_fn so smp_parallel_sum can split it across cores. */
long smp_bench_primes(long lo, long hi, int core)
{
    (void)core;
    if (lo < 2) lo = 2;
    long cnt = 0;
    for (long n = lo; n < hi; n++) {
        int prime = 1;
        for (long d = 2; d * d <= n; d++) {
            if (n % d == 0) { prime = 0; break; }
        }
        cnt += prime;
    }
    return cnt;
}

void smp_run_boot_bench(long n)
{
    if (n < 1) n = 1;
    int online = smp_cores_online();

    unsigned long t0 = smp_now_ms();
    long c1 = smp_parallel_sum(smp_bench_primes, n, 1);
    unsigned long t1 = smp_now_ms();
    long cN = smp_parallel_sum(smp_bench_primes, n, online);
    unsigned long t2 = smp_now_ms();

    unsigned long ms1 = t1 - t0, msN = t2 - t1;
    uart_puts("smp-bench: primes in [0,"); uart_dec((unsigned long)n);
    uart_puts(") = "); uart_dec((unsigned long)c1);
    uart_puts((c1 == cN) ? " (1-core == N-core, ok)\n" : " MISMATCH!\n");
    uart_puts("  1-core   ms = "); uart_dec(ms1); uart_puts("\n");
    uart_puts("  ");           uart_dec((unsigned long)online);
    uart_puts("-core   ms = "); uart_dec(msN); uart_puts("\n");
    uart_puts("  speedup x100 = ");
    uart_dec(msN ? (ms1 * 100UL) / msN : 0);
    uart_puts("  (e.g. 385 = 3.85x)\n");
}
