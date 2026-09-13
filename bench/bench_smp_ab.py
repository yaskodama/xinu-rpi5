#!/usr/bin/env python3
"""bench_smp_ab.py —— Pi 5 の「ワーカーズ型」対「対称 SMP 型」を同じ板・同じ起動で比べる。

  A. ワーカーズ型（郵便箱・静的分割）  GET /bench?kind=nqueens&n=&cores=
     核 0 が各二次コアの郵便箱に仕事を置き、範囲を**あらかじめ**均等に割る
     (system/smp.c: smp_parallel_sum → smp_worker_poll_once)。

  B. 対称 SMP 型（共有 ready キュー）  GET /smpsched?n=&tasks=
     プロセスを tasks 本作って ready キューへ入れ、**各コアが自分で取る**
     (system/smpsched.c)。偏りは走行中に均される。

同じ N-Queens を解くので直接比べられる。同じカーネルの同じ起動のまま両方を
走らせられる（smp_worker_poll_once が対称版にも郵便箱の口を残しているため）＝
焼き直し・再起動・温度・過渡といった交絡なしに A/B が取れる。

測り方の作法:
  * 先に計器を確かめる（/version の build、/smplock のスピンロック自己診断、
    cores_online）。計器が壊れていれば数字に意味がない。
  * 暖機を捨てる（起動直後の過渡を数に入れない）。
  * A と B を**交互に**繰り返す（片方だけが熱くなる/冷えるのを避ける）。
  * 平均でなく**中央値と最小・最大**を出す。速さの主張は分布で見る。
  * 解の個数が既知の値と一致することを毎回確かめる（速いが間違っている、を弾く）。

使い方:
  python3 bench/bench_smp_ab.py [--host 192.168.3.101] [--n 12] [--reps 7]
"""
import argparse, re, statistics, sys, time, urllib.request

NQ_KNOWN = {8: 92, 9: 352, 10: 724, 11: 2680, 12: 14200, 13: 73712, 14: 365596}


def get(host, path, timeout=180):
    url = f"http://{host}{path}"
    t0 = time.time()
    with urllib.request.urlopen(url, timeout=timeout) as r:
        body = r.read().decode("utf-8", "replace")
    return body, time.time() - t0


def num(text, key, cast=int):
    m = re.search(re.escape(key) + r"\s*=?\s*([0-9-]+)", text)
    return cast(m.group(1)) if m else None


def workers(host, n, cores):
    """A: 郵便箱・静的分割。1コア直列と cores コア並列の両方を返す[us]。"""
    t, _ = get(host, f"/bench?kind=nqueens&n={n}&cores={cores}")
    return {"us1": num(t, "1-core   us"), "usN": num(t, "N-core   us"),
            "sol": num(t, "solutions"), "agree": "agree = yes" in t,
            "cores": num(t, "cores_online"), "raw": t.strip()}


def smpsched(host, n, tasks):
    """B: 対称 SMP スケジューラ。並列の makespan[ms] と コア別の実行本数。"""
    t, _ = get(host, f"/smpsched?n={n}&tasks={tasks}")
    per = re.search(r"ran_per_core=([0-9/]+)", t)
    return {"ms": num(t, "ms"), "sol": num(t, "solutions"),
            "per": [int(x) for x in per.group(1).split("/")] if per else [],
            "raw": t.strip()}


def stat(xs):
    return (statistics.median(xs), min(xs), max(xs))


def workers_only(H, a):
    """対称SMP の口が無いカーネル向け: ワーカーズ型（郵便箱・静的分割）だけを測る。"""
    cores = workers(H, 8, 0)["cores"]
    n = a.n or 13
    print(f"\n== ワーカーズ型のみ / N-Queens n={n}（解 {NQ_KNOWN.get(n,'?')}）/ cores_online={cores} ==")
    print("  暖機（捨てる）…", end="", flush=True); workers(H, n, cores); print(" 済")

    print(f"\n-- コア数の掃引（Amdahl）: 繰返し {a.reps} 回、中央値 [最小..最大] --")
    base = None
    for c in range(1, cores + 1):
        par, ser = [], []
        for _ in range(a.reps):
            r = workers(H, n, c)
            par.append(r["usN"] / 1000.0); ser.append(r["us1"] / 1000.0)
            if NQ_KNOWN.get(n) not in (None, r["sol"]) or not r["agree"]:
                print("  ⚠ 解が合わない:", r["raw"].replace("\n", " ")); 
        pm, pn, px = stat(par); sm, *_ = stat(ser)
        if base is None: base = sm
        print(f"  cores={c}  {pm:8.1f} ms  [{pn:7.1f}..{px:7.1f}]   直列 {sm:7.1f} ms   "
              f"速度向上 {sm/pm:4.2f}x   効率 {100*sm/pm/c:3.0f}%")

    print(f"\n-- 負荷の種類による違い（{cores}コア、繰返し {a.reps} 回の中央値）--")
    print(f"  {'負荷':<10} {'直列 ms':>9} {'並列 ms':>9} {'速度向上':>8}   一致")
    for path, label in [(f"/bench?kind=nqueens&n={n}", f"nqueens n={n}"),
                        ("/bench?kind=primes&n=300000", "primes 30万"),
                        ("/bench?kind=dining&n=5", "dining 5人"),
                        ("/bench?kind=fill&n=200000", "fill 20万語")]:
        ser, par, ag = [], [], True
        for _ in range(a.reps):
            t, _t = get(H, path)
            ser.append(num(t, "1-core   us") / 1000.0); par.append(num(t, "N-core   us") / 1000.0)
            ag = ag and ("agree = yes" in t)
        sm, *_ = stat(ser); pm, *_ = stat(par)
        print(f"  {label:<10} {sm:9.2f} {pm:9.2f} {sm/pm:7.2f}x   {'yes' if ag else 'NO'}")
    print("\n※ 対称SMP型（共有 ready キュー / /smpsched）はこのカーネルに無いため未測定。")
    return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.3.101")
    ap.add_argument("--n", type=int, default=0, help="0 なら実測して手頃な盤面を選ぶ")
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--tasks", type=int, default=0, help="0 なら n（=列数）")
    a = ap.parse_args()
    H = a.host

    print("== 計器の確認 ==")
    ver, _ = get(H, "/version")
    print("  " + ver.strip().splitlines()[0])
    lock, _ = get(H, "/smplock?n=20000")
    have_sym = "404" not in lock
    print("  " + lock.strip())
    if have_sym and "OK" not in lock:
        sys.exit("スピンロックの自己診断が通らない。ここが壊れていれば測っても意味がない。")
    if not have_sym:
        # 対称 SMP スケジューラ（/smpsched・/smplock）を持たないカーネル。
        # A/B は取れないので、測れる側（ワーカーズ型）だけを測って正直にそう言う。
        print("  ※ この版に /smpsched が無い → 対称SMP型は測れない。ワーカーズ型のみ測る")

    if not have_sym:
        return workers_only(H, a)

    n = a.n
    if not n:                      # 直列 0.3〜3 秒に収まる盤面を選ぶ
        for cand in (10, 11, 12, 13, 14):
            r = workers(H, cand, 1)
            print(f"  下見 n={cand}: 直列 {r['us1']/1000:.0f} ms")
            n = cand
            if r["us1"] > 300_000:
                break
    tasks = a.tasks or n
    cores = workers(H, 8, 0)["cores"]
    print(f"\n== N-Queens n={n}（解 {NQ_KNOWN.get(n,'?')}）/ cores_online={cores} / 繰返し {a.reps} 回 ==")

    print("  暖機（捨てる）…", end="", flush=True)
    workers(H, n, cores); smpsched(H, n, tasks)
    print(" 済")

    A, B, ser, per_core = [], [], [], []
    bad = []
    for i in range(a.reps):        # A と B を交互に（片側だけが温まらないように）
        w = workers(H, n, cores)
        s = smpsched(H, n, tasks)
        A.append(w["usN"] / 1000.0); ser.append(w["us1"] / 1000.0)
        B.append(float(s["ms"])); per_core.append(s["per"])
        if NQ_KNOWN.get(n) not in (None, w["sol"]): bad.append(f"A#{i} solutions={w['sol']}")
        if NQ_KNOWN.get(n) not in (None, s["sol"]): bad.append(f"B#{i} solutions={s['sol']}")
        if not w["agree"]: bad.append(f"A#{i} 直列と並列で解が不一致")
        print(f"  {i+1}/{a.reps}  workers {A[-1]:7.1f} ms   smpsched {B[-1]:7.1f} ms"
              f"   (直列 {ser[-1]:7.1f} ms, コア別 {'/'.join(map(str,s['per']))})")

    ms, mn, mx = stat(ser);  print(f"\n直列(1コア)          中央値 {ms:8.1f} ms  [{mn:.1f}..{mx:.1f}]")
    am, an, ax = stat(A);    print(f"A ワーカーズ({cores}コア) 中央値 {am:8.1f} ms  [{an:.1f}..{ax:.1f}]  速度向上 {ms/am:4.2f}x")
    bm, bn, bx = stat(B);    print(f"B 対称SMP({cores}コア)    中央値 {bm:8.1f} ms  [{bn:.1f}..{bx:.1f}]  速度向上 {ms/bm:4.2f}x")
    print(f"\nB / A = {bm/am:4.2f}倍（1.00 より小さければ 対称SMP のほうが速い）")
    if per_core:
        print("コア別に走ったタスク数（B）:", " | ".join("/".join(map(str, p)) for p in per_core[:5]))
    if bad:
        print("\n⚠ 正しさの検査に引っかかった:", "; ".join(bad))
    else:
        print("\n解の個数は毎回 既知の値と一致（速いが間違っている、ではない）")

    print(f"\n== 参考: A のコア数掃引（Amdahl）n={n} ==")
    for c in range(1, (cores or 4) + 1):
        r = workers(H, n, c)
        print(f"  cores={c}  {r['usN']/1000:8.1f} ms   速度向上 {r['us1']/r['usN']:4.2f}x")
    print(f"\n== 参考: B のタスク数掃引 n={n}（静的分割との差が出る所）==")
    for t in sorted({1, 2, 4, cores or 4, n, 2 * n}):
        if t < 1: continue
        r = smpsched(H, n, t)
        print(f"  tasks={t:3d}  {r['ms']:8.1f} ms   コア別 {'/'.join(map(str,r['per']))}")


if __name__ == "__main__":
    main()
