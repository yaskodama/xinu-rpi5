// include/critical.h — tiny IRQ-masking critical sections (ported from rpi4).
//
// With preemption on, the 100 Hz timer IRQ can fire at any instruction and
// re-enter the scheduler.  Wrap ready-list / sleeper mutations in
// irq_save()/irq_restore() so the timer can't corrupt them.  save returns the
// prior DAIF so the pair nests correctly (e.g. when already inside an IRQ).

#ifndef XINU_RPI5_CRITICAL_H
#define XINU_RPI5_CRITICAL_H

static inline unsigned long irq_save(void)
{
    unsigned long d;
    __asm__ volatile ("mrs %0, daif; msr daifset, #2" : "=r"(d) :: "memory");
    return d;
}

static inline void irq_restore(unsigned long d)
{
    __asm__ volatile ("msr daif, %0" :: "r"(d) : "memory");
}

#endif /* XINU_RPI5_CRITICAL_H */
