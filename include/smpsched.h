// include/smpsched.h — 対称 SMP スケジューラ（比較実験用の第2方式）。
//
// 既定のカーネルは「ワーカ郵便箱」方式である ―― 核0 が OS 全体を回し、
// 核1〜3 は WFE で待って、投函された範囲だけを計算する（include/smp.h）。
// 分配は投函時に静的に決まるので、仕事量が偏ると最も重い核が makespan を決める。
//
// こちらは対称 SMP である ―― **4 コアすべてがスケジューラを回し**、
// 共有 ready キューから自分でプロセスを取る。偏りは走行中に均される。
//
// 範囲（この版で意図的にやらないこと）:
//   - デバイス・アクタ・ネットワーク・画面は従来どおり核0 だけが触る。
//     これらは再入可能でないので、対称化の対象は**スケジューラだけ**に限る。
//     核1〜3 で走るプロセスは計算して終了するもの（getmem/uart/net を呼ばない）。
//   - 核1〜3 にはタイマ割り込みを配っていないので、そこで走るプロセスは
//     自発的に終了するまで走る（ノンプリエンプティブ）。計算タスクの比較には十分。
//
// この二つの制限は論文で明示すること。「対称にした部分」と「していない部分」を
// 曖昧にすると比較の意味が消える。
#ifndef XINU_RPI5_SMPSCHED_H
#define XINU_RPI5_SMPSCHED_H

#include "smp.h"

#ifdef SMP_SYMMETRIC

/* 各コアが現在走らせている pid（proc.h の currpid はこれの別名）。 */
extern int smpsched_curr[SMP_NCORES];

/* 核1〜3 用の idle プロセスの pid（proctab の後ろから予約する）。 */
#define SMPIDLE_BASE (NPROC - (SMP_NCORES - 1))
#define SMPIDLE_PID(core) (SMPIDLE_BASE + (core) - 1)

void smpsched_init(void);            /* proc_init から呼ぶ */
void smpsched_core_loop(int core);   /* 核1〜3 の本体（戻らない） */
void smpsched_after_switch(void);    /* 新規プロセスの入口でロックを離す */

/* ---- 比較実験 ---------------------------------------------------------
 * 同じ N-Queens を「対称スケジューラ上の ntasks 個のプロセス」として走らせる。
 * ntasks を列数より多くすると、動的な奪い合いで偏りが均される。
 * 戻り値 = 解の総数。*ms に経過時間、*ran に各コアが実際に処理したタスク数。 */
long smpsched_nqueens(int n, int ntasks, unsigned long *ms, int ran[SMP_NCORES]);

#endif /* SMP_SYMMETRIC */
#endif
