// include/smp.h — worker-pool SMP for the BCM2712's 4 Cortex-A76 cores.
//
// Architecture (ported from xinu-rpi4's BCM2711 4×A72 SMP):
//   - Core 0 keeps running the whole single-core cooperative OS (scheduler,
//     ready list, actors, net, USB, video) UNCHANGED.  The ready list stays
//     core-0-private — the AIPL/actor/net/USB/video runtime is non-reentrant,
//     so a shared symmetric run queue would destabilise it.
//   - Cores 1-3 are brought up as dedicated compute workers: they idle in WFE
//     and run a posted job (a range of work) in parallel, then signal done.
//   - Coordination is lock-free: with the D-cache OFF (SCTLR.C=0, see mmu.c —
//     mmu_init / mmu_enable_secondary enable M only) every access goes straight
//     to RAM, so plain `volatile` + `dmb`/`dsb` barriers are coherent across
//     cores — no cache protocol, no spinlocks.

#ifndef SMP_H
#define SMP_H

#define SMP_NCORES 4

/* Bring up cores 1-3 (call once from core 0 after mmu_init/exception_init +
 * gic/timer).  Safe to call even if the secondary cores never respond — they
 * just stay offline and parallel jobs fall back to running on core 0. */
void smp_init(void);

/* Number of cores that came online and are ready for work (1..4). */
int  smp_cores_online(void);

/* MPIDR_EL1[1:0] of the calling core. */
int  smp_core_id(void);

/* A unit of work: compute a partial result over the half-open range [lo,hi).
 * `core` is the executing core id (0..3), e.g. for per-core scratch. */
typedef long (*smp_range_fn)(long lo, long hi, int core);

/* Split [0,n) into `ncores` contiguous chunks, run `fn` on each chunk in
 * parallel across cores 0..ncores-1 (core 0 runs its own chunk inline), and
 * return the sum of the partial results.  Blocks until every chunk is done.
 * A worker that does not finish within a bounded spin is taken over by core 0
 * so the result is always correct (a hung worker can never wedge the OS). */
long smp_parallel_sum(smp_range_fn fn, long n, int ncores);

#endif /* SMP_H */
