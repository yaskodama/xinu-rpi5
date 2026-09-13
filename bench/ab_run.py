#!/usr/bin/env python3
"""ab_run.py —— Pi 5「ワーカーズ型」対「対称SMP型」を同じ起動のまま測る本番実行。

段取り（この順でないと意味が変わる）:
  0. 計器を確かめる          /version・/smplock（ロックが壊れていれば数字は無意味）
  1. 切り替える前に A を測る  /bench?kind=nqueens&cores=1..4（郵便箱・静的分割）
     ついでに /smpsched も測る（このとき二次コアは郵便箱に居るので＝実質1コア）
  2. /smpmode?on=1 で対称スケジューラへ移す（一方通行。戻すには電源再投入）
  3. 生存を確かめてから A と B を**交互に**測る（片側だけ温まるのを避ける）
  4. 中央値・最小・最大で出す。解の個数は毎回 既知の値と突き合わせる

使い方: python3 bench/ab_run.py [--host 192.168.3.101] [--n 13] [--reps 7]
"""
import argparse, json, re, statistics, sys, time, urllib.request

NQ = {8: 92, 9: 352, 10: 724, 11: 2680, 12: 14200, 13: 73712, 14: 365596}


def get(host, path, timeout=180):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def num(t, key, cast=int):
    m = re.search(re.escape(key) + r"\s*=?\s*([0-9-]+)", t)
    return cast(m.group(1)) if m else None


def bench(host, n, cores):                      # A: 郵便箱・静的分割
    t = get(host, f"/bench?kind=nqueens&n={n}&cores={cores}")
    return {"us1": num(t, "1-core   us"), "usN": num(t, "N-core   us"),
            "sol": num(t, "solutions"), "ok": "agree = yes" in t}


def sched(host, n, tasks):                      # B: 共有 ready キュー
    t = get(host, f"/smpsched?n={n}&tasks={tasks}")
    per = re.search(r"ran_per_core=([0-9/]+)", t)
    return {"ms": num(t, "ms"), "sol": num(t, "solutions"),
            "per": [int(x) for x in per.group(1).split("/")] if per else []}


def med(xs): return (statistics.median(xs), min(xs), max(xs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.3.101")
    ap.add_argument("--n", type=int, default=13)
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--switch", action="store_true", help="対称スケジューラへ切り替える")
    a = ap.parse_args()
    H, n, R = a.host, a.n, a.reps
    out = {"host": H, "n": n, "reps": R}

    print("== 0. 計器 ==")
    ver = get(H, "/version").splitlines()[0]
    lock = get(H, f"/smplock?n=20000").splitlines()[0]
    mode = get(H, "/smpmode").splitlines()[0]
    print(f"  {ver}\n  {lock}\n  {mode}")
    if "OK" not in lock:
        sys.exit("ロックの自己診断が通らない。ここが壊れていれば測る意味がない。")
    out["version"], out["lock"], out["mode_before"] = ver, lock, mode

    print(f"\n== 1. 切り替え前: ワーカーズ型（郵便箱・静的分割）N-Queens n={n} ==")
    bench(H, n, 4)                                            # 暖機（捨てる）
    print(f"  {'コア':>4} {'並列 ms':>10} {'[最小..最大]':>20} {'直列 ms':>10} {'速度向上':>8} {'効率':>6}")
    out["A"] = {}
    for c in range(1, 5):
        par, ser = [], []
        for _ in range(R):
            r = bench(H, n, c)
            assert r["sol"] == NQ[n] and r["ok"], f"解が合わない: {r}"
            par.append(r["usN"] / 1000); ser.append(r["us1"] / 1000)
        pm, pn, px = med(par); sm, *_ = med(ser)
        out["A"][c] = {"par_ms": pm, "min": pn, "max": px, "ser_ms": sm}
        print(f"  {c:>4} {pm:10.1f} {f'[{pn:.1f}..{px:.1f}]':>20} {sm:10.1f} "
              f"{sm/pm:7.2f}x {100*sm/pm/c:5.0f}%")

    print(f"\n== 1b. 同じ仕事を対称スケジューラの口で（まだ二次コアは郵便箱側）==")
    s = [sched(H, n, n)["ms"] for _ in range(3)]
    out["B_before_switch_ms"] = med(s)[0]
    print(f"  /smpsched tasks={n}: 中央値 {med(s)[0]:.1f} ms  {s}")

    if not a.switch:
        print("\n（--switch を付けると対称スケジューラへ移して A/B を取ります）")
        json.dump(out, open("/tmp/ab_phase1.json", "w"), ensure_ascii=False, indent=1)
        return

    print("\n== 2. 対称スケジューラへ切り替え ==")
    print("  " + get(H, "/smpmode?on=1").replace("\n", "\n  ").rstrip())
    time.sleep(1)
    alive = get(H, "/version").splitlines()[0]
    print(f"  生存確認: {alive}")
    out["mode_after"] = get(H, "/smpmode").splitlines()[0]

    print(f"\n== 3. A と B を交互に（{R} 回）==")
    A, B, S, per = [], [], [], []
    for i in range(R):
        ra = bench(H, n, 4); rb = sched(H, n, n)
        assert ra["sol"] == NQ[n] and ra["ok"], f"A の解が合わない: {ra}"
        assert rb["sol"] == NQ[n], f"B の解が合わない: {rb}"
        A.append(ra["usN"] / 1000); S.append(ra["us1"] / 1000)
        B.append(float(rb["ms"])); per.append(rb["per"])
        print(f"  {i+1}/{R}  A(ワーカーズ) {A[-1]:7.1f} ms   B(対称SMP) {B[-1]:7.1f} ms"
              f"   直列 {S[-1]:7.1f} ms   コア別 {'/'.join(map(str,rb['per']))}")
    sm, sn, sx = med(S); am, an, ax = med(A); bm, bn, bx = med(B)
    out["final"] = {"serial": [sm, sn, sx], "workers": [am, an, ax], "symmetric": [bm, bn, bx],
                    "per_core": per}
    print(f"\n  直列(1コア)      {sm:8.1f} ms  [{sn:.1f}..{sx:.1f}]")
    print(f"  A ワーカーズ型   {am:8.1f} ms  [{an:.1f}..{ax:.1f}]   速度向上 {sm/am:4.2f}x")
    print(f"  B 対称SMP型      {bm:8.1f} ms  [{bn:.1f}..{bx:.1f}]   速度向上 {sm/bm:4.2f}x")
    print(f"  B/A = {bm/am:4.2f}（1 未満なら対称SMPが速い）")

    print(f"\n== 4. B のタスク数掃引（静的分割との差が出る所）==")
    out["sweep"] = {}
    for t in (1, 2, 4, 8, n, 2 * n):
        r = sched(H, n, t)
        out["sweep"][t] = r["ms"]
        print(f"  tasks={t:3d}  {r['ms']:8.1f} ms   コア別 {'/'.join(map(str,r['per']))}")
    json.dump(out, open("/tmp/ab_result.json", "w"), ensure_ascii=False, indent=1)
    print("\n(生データ: /tmp/ab_result.json)")


if __name__ == "__main__":
    main()
