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

#define TIMER_IRQ_PPI 30

static unsigned long          timer_interval;
static volatile unsigned long tick_count;

static inline void cntp_set_tval(unsigned long v)
{
    __asm__ volatile ("msr cntp_tval_el0, %0" : : "r"(v));
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

static void timer_irq_handler(void *arg)
{
    (void)arg;
    /* Re-arm before incrementing so we don't drift if the rest
     * of the handler takes a few μs. */
    cntp_set_tval(timer_interval);
    tick_count++;
}

void timer_init(void)
{
    unsigned long freq = cntfrq();
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
