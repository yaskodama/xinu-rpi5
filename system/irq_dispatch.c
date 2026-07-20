// system/irq_dispatch.c — top-level IRQ dispatcher (phase S1d).
//
// Glue between the asm stub `irq_entry` (in exception_vectors.S)
// and per-source handler functions registered via
// connect_interrupt().  Handler table is a flat array indexed by
// the full GIC interrupt ID — fine at 256 entries since BCM2711
// has well under that many SPIs we care about.

#include "irq.h"
#include "gic.h"
#include "proc.h"

#define IRQ_TABLE_MAX 256

static irq_handler_t handlers[IRQ_TABLE_MAX];
static void         *handler_args[IRQ_TABLE_MAX];

void connect_interrupt(unsigned irq, irq_handler_t fn, void *arg)
{
    if (irq >= IRQ_TABLE_MAX) return;
    handlers[irq]     = fn;
    handler_args[irq] = arg;
}

void irq_dispatch_c(void)
{
    unsigned int iar = gic_ack();
    unsigned int id  = iar & 0x3FFu;

    if (id == 1023u) {
        /* Spurious — no IRQ was actually pending.  No EOI needed. */
        return;
    }

    if (id < IRQ_TABLE_MAX && handlers[id]) {
        handlers[id](handler_args[id]);
    }
    /* Even an unconnected SPI/PPI must be EOI'd so the GIC moves
     * past it; otherwise the same ID re-fires forever. */
    gic_eoi(iar);

    /* After EOI (so the next process can still take timer IRQs), do any
     * pending preemptive context switch.  No-op unless preemption is enabled. */
    proc_preempt();
}
