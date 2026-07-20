// device/timer/timer.c — ARM generic timer, 100 Hz periodic tick.
//
// IRQ source: PPI 30 (CNTPNS — physical non-secure timer).  At
// EL1 non-secure on BCM2711 this is the only timer we have
// direct access to; CNTV / CNTHP need EL2.
//
// Programming model:
//   - CNTFRQ_EL0  read-only counter frequency in Hz
//   - CNTP_TVAL_EL0  signed countdown register; IRQ fires when ≤ 0
//   - CNTP_CTL_EL0   bit0 = ENABLE, bit1 = IMASK, bit2 = ISTATUS
//
// On every IRQ we reload CNTP_TVAL_EL0 with the same interval to
// make it periodic.  No drift compensation — good enough for a
// 100 Hz scheduler / USPi `StartKernelTimer` substrate.

#include "timer.h"
#include "irq.h"
#include "gic.h"
#include "proc.h"

#define TIMER_IRQ_PPI 30

static unsigned long          timer_interval;
static unsigned long          timer_freq;     /* CNTFRQ_EL0 (ticks/sec) */
static volatile unsigned long tick_count;

static inline void cntp_set_tval(unsigned long v)
{
    __asm__ volatile ("msr cntp_tval_el0, %0" : : "r"(v));
}

static inline long cntp_get_tval(void)
{
    long v;
    __asm__ volatile ("mrs %0, cntp_tval_el0" : "=r"(v));
    return v;
}

static inline unsigned long us_to_ticks(unsigned long us)
{
    return (timer_freq * us) / 1000000UL;
}

/* Tickless one-shot helper: arm the timer to fire `us` from now if sooner than
 * what is pending.  Called (IRQs masked) from proc_sleep_us for precise RT
 * wakeup.  (Stage 1 keeps the periodic reload; this just lets a nearer RT
 * deadline pull the next fire in.) */
void timer_arm_before_us(unsigned long us)
{
    if (!timer_freq) return;
    long want = (long)us_to_ticks(us);
    if (want < cntp_get_tval()) cntp_set_tval((unsigned long)(want > 1 ? want : 1));
}

static inline void cntp_set_ctl(unsigned long v)
{
    __asm__ volatile ("msr cntp_ctl_el0, %0" : : "r"(v));
}

static inline unsigned long cntfrq(void)
{
    unsigned long v;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

/* Optional per-tick hook, run inside the timer IRQ.  The default is a
 * no-op; loader/main.c overrides it to drain the Pi 5 RP1 GEM RX ring at
 * a guaranteed 100 Hz (the wm render loop was too slow, so RX overran and
 * BNA-froze).  Weak so Pi 4 / QEMU builds that don't define it still link. */
__attribute__((weak)) void timer_tick_hook(void) {}

static void timer_irq_handler(void *arg)
{
    (void)arg;
    /* Re-arm before incrementing so we don't drift if the rest
     * of the handler takes a few μs. */
    cntp_set_tval(timer_interval);
    tick_count++;
    proc_timer_tick();         /* wake due RT sleepers (no-op if none) */
    timer_tick_hook();         /* drain the net stack (unchanged in Stage 1) */
    proc_resched_request();    /* request a preemptive switch (acted on after EOI) */
}

void timer_init(void)
{
    unsigned long freq = cntfrq();
    timer_freq     = freq;
    timer_interval = freq / TIMER_HZ;

    /* Mask + disable while we configure so a partial state can't
     * fire an unexpected IRQ. */
    cntp_set_ctl(2);                /* IMASK=1, ENABLE=0 */
    cntp_set_tval(timer_interval);

    connect_interrupt(TIMER_IRQ_PPI, timer_irq_handler, 0);
    gic_enable_irq(TIMER_IRQ_PPI);

#ifdef GIC_BASE
    /* ENABLE=1, IMASK=0 — IRQ now pending in CNTP, will fire as
     * soon as the CPU unmasks DAIF.I (caller's responsibility). */
    cntp_set_ctl(1);
#else
    /* No reachable GIC (Pi 5: 41-bit GIC unmapped with MMU off).  Leave
     * the timer IRQ MASKED so an undeliverable/un-acked PPI can't livelock
     * the CPU; the free-running CNTPCT counter still drives timer_ticks(). */
    cntp_set_ctl(2);                /* ENABLE=0, IMASK=1 */
#endif
}

/* Raw count of timer IRQs actually taken (0 = the PPI is never delivered,
 * i.e. the GIC/timer IRQ path is dead).  Distinct from timer_ticks(), which
 * falls back to the polled CNTPCT counter when no IRQ ever fires. */
unsigned long timer_irq_count(void)
{
    return tick_count;
}

unsigned long timer_ticks(void)
{
    /* Pi 5: the GIC (0x1007FFF9000) is a 41-bit address, unreachable with
     * the MMU off (40-bit PA limit -> address-size fault), so the timer
     * PPI is never delivered and tick_count never advances.  Derive a live
     * tick from the always-readable generic-timer COUNTER instead — no GIC,
     * no memory access, just a system register.  (Pi 4's GIC is 32-bit and
     * the real IRQ path keeps tick_count authoritative there.) */
    if (tick_count) return tick_count;          /* real IRQ path is live */
    unsigned long cnt, frq;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(cnt));
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(frq));
    if (frq < TIMER_HZ) return cnt;             /* avoid div-by-zero      */
    return cnt / (frq / TIMER_HZ);              /* polled 100 Hz uptime   */
}
