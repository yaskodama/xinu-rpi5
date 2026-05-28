// device/gic/gic.c — minimal GIC-400 driver for BCM2711.
//
// Implements the small subset needed to take a periodic generic
// timer tick (PPI 30) plus the DWC2 USB IRQ (SPI 105) once the
// USPi adapter is wired.
//
// Register map (all offsets relative to the documented base):
//
// GICD (distributor) @ GIC_BASE + 0x1000:
//   0x000 GICD_CTLR       — bit0 = enable group0
//   0x080 GICD_IGROUPRn   — 32 IRQs per word (1 = group1 / non-secure)
//   0x100 GICD_ISENABLERn — 32 IRQs per word (1 = enable, write-only-set)
//   0x180 GICD_ICENABLERn — disable (write-only-clear)
//   0x200 GICD_ISPENDRn   — set-pending
//   0x280 GICD_ICPENDRn   — clear-pending
//   0x400 GICD_IPRIORITYRn — 8-bit priority per IRQ (lower = higher prio)
//   0x800 GICD_ITARGETSRn — 8-bit core target per IRQ (SPI only)
//   0xC00 GICD_ICFGRn    — 2 bits per IRQ: 0 = level, 2 = edge (SPI 32+)
//
// GICC (CPU interface) @ GIC_BASE + 0x2000:
//   0x000 GICC_CTLR  — bit0 = enable
//   0x004 GICC_PMR   — priority mask (we set 0xF0 = allow all)
//   0x008 GICC_BPR
//   0x00C GICC_IAR   — read returns active IRQ ID
//   0x010 GICC_EOIR  — write to retire

#include "gic.h"

#ifdef GIC_BASE

/* GIC-400 (Pi 4): GICD at +0x1000, GICC at +0x2000.  QEMU virt GICv2:
 * GICD at +0x0, GICC at +0x10000.  Overridable per target. */
#ifndef GICD_OFFSET
#define GICD_OFFSET 0x1000UL
#endif
#ifndef GICC_OFFSET
#define GICC_OFFSET 0x2000UL
#endif
#define GICD_BASE   (GIC_BASE + GICD_OFFSET)
#define GICC_BASE   (GIC_BASE + GICC_OFFSET)

#define GICD_CTLR        (*(volatile unsigned int *)(GICD_BASE + 0x000))
#define GICD_IGROUPR(n)  (*(volatile unsigned int *)(GICD_BASE + 0x080 + 4 * (n)))
#define GICD_ISENABLER(n)(*(volatile unsigned int *)(GICD_BASE + 0x100 + 4 * (n)))
#define GICD_ICENABLER(n)(*(volatile unsigned int *)(GICD_BASE + 0x180 + 4 * (n)))
#define GICD_IPRIORITYR(n)(*(volatile unsigned int *)(GICD_BASE + 0x400 + 4 * (n)))
#define GICD_ITARGETSR(n)(*(volatile unsigned int *)(GICD_BASE + 0x800 + 4 * (n)))
#define GICD_ICFGR(n)    (*(volatile unsigned int *)(GICD_BASE + 0xC00 + 4 * (n)))

#define GICC_CTLR        (*(volatile unsigned int *)(GICC_BASE + 0x000))
#define GICC_PMR         (*(volatile unsigned int *)(GICC_BASE + 0x004))
#define GICC_IAR         (*(volatile unsigned int *)(GICC_BASE + 0x00C))
#define GICC_EOIR        (*(volatile unsigned int *)(GICC_BASE + 0x010))

void gic_init(void)
{
    /* Disable distributor while we reset the per-IRQ tables. */
    GICD_CTLR = 0;

    /* Clear-enable everything (32 banks × 32 = 1024 IRQs).  Each
     * ICENABLER bit is write-1-to-clear so we can OR-write 0xFFFFFFFF
     * regardless of current state without races. */
    for (int n = 0; n < 32; n++) GICD_ICENABLER(n) = 0xFFFFFFFFU;

    /* Default priority = 0xA0 for every IRQ (mid-range).  Lower
     * numerical value = higher priority; PMR will be set so 0xF0
     * is the cutoff. */
    for (int n = 0; n < 256; n++) GICD_IPRIORITYR(n) = 0xA0A0A0A0U;

    /* SPI targets default to core 0 (mask 0x01 per byte).  Only
     * SPIs (n >= 8 since first 32 are SGI/PPI not addressable here)
     * — but writing the low banks is harmless on GIC-400. */
    for (int n = 8; n < 256; n++) GICD_ITARGETSR(n) = 0x01010101U;

    /* Put every IRQ in GROUP 1.  At non-secure EL1 (where Pi 4 boots), only
     * group-1 interrupts are signalled as IRQ; group-0 would be routed as a
     * (secure) FIQ, which we keep masked — so a group-0 timer IRQ never
     * arrives.  This was why the 100 Hz tick never fired. */
    for (int n = 0; n < 32; n++) GICD_IGROUPR(n) = 0xFFFFFFFFU;

    /* Enable the distributor.  In the (banked) NS view bit0 = EnableGrp1;
     * set bit1 too so it also works if we are presented the single-view
     * (DS=1) layout where bit1 = EnableGrp1. */
    GICD_CTLR = 3;

    /* CPU interface: priority mask wide open, enable group 1 forwarding. */
    GICC_PMR  = 0xF0;
    GICC_CTLR = 1;
}

void gic_enable_irq(unsigned irq)
{
    GICD_ISENABLER(irq / 32) = 1U << (irq % 32);
}

void gic_disable_irq(unsigned irq)
{
    GICD_ICENABLER(irq / 32) = 1U << (irq % 32);
}

unsigned int gic_ack(void)
{
    return GICC_IAR;
}

void gic_eoi(unsigned int iar)
{
    GICC_EOIR = iar;
}

#endif  /* GIC_BASE */
