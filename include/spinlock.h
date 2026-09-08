// include/spinlock.h — 対称 SMP 用のスピンロック。
//
// この板は SCTLR.C=0（D-cache OFF）で走っている。すべてのアクセスが RAM に
// 抜けるので「見え方」の問題は無いが、**排他ロード/ストア（LDAXR/STXR）が
// 非キャッシュ可能領域で成立するか**は実装依存である。したがって二本用意し、
// 実機で確かめてから選ぶ:
//
//   SPIN_EXCLUSIVE (既定) : LDAXR/STXR のチケットロック。速いが要検証
//   SPIN_BAKERY           : Lamport のパン屋アルゴリズム。素の load/store と
//                           バリアだけで成立するので、排他が効かなくても正しい
//
// どちらが正しく動くかは /smplock で実機判定する（4コアで共有カウンタを
// 叩き、期待値と一致するか）。判定してから既定を決めること。
#ifndef XINU_RPI5_SPINLOCK_H
#define XINU_RPI5_SPINLOCK_H

#include "smp.h"

struct spinlock {
    /* exclusive 版 */
    volatile unsigned int next_ticket;
    volatile unsigned int now_serving;
    /* bakery 版 */
    volatile unsigned char choosing[SMP_NCORES];
    volatile unsigned int  number[SMP_NCORES];
    /* 計測用（どちらの版でも数える） */
    volatile unsigned long acquires;
    volatile unsigned long spins;
    const char *name;
};

void spin_init(struct spinlock *l, const char *name);
void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

/* 実機での自己診断: 4 コアが n 回ずつ共有カウンタを ++ する。
 * 戻り値 = 実測カウンタ。期待値と違えばロックが壊れている。 */
long spin_selftest(int per_core);
const char *spin_impl_name(void);

#endif
