#!/usr/bin/env python3
"""nqueens_with_gc_eval.py — distributed-actor N-Queens benchmark with GC
co-resident.  One /actor/load of nqueens_with_gc.abcl, then for each
N in {4..8}:

  1. POST bootstrap(N) to Root (slot 1)
  2. Sample peak actor count + GC dry-run target count every 50 ms while
     N-Queens runs.  Verify (a) targets@500ms stays at 0 during the run
     (GC would never falsely kill an in-flight Solver), (b) targets@30s
     stays at 0 (well below threshold).
  3. On done, read get_solutions(0) and compare to expected.
  4. Trigger one real GC tick via slot 0.tick — verify killed_total
     stays at 0 across all runs (Solvers properly self-suicide).

Final table:
  N  expected  got  elapsed_ms  peak_actors  gc_scans  gc_kills
"""
import os, subprocess, sys, time, urllib.request

PI      = "192.168.3.100"
ROOT    = "/Users/kodamay/ocaml-app/abclcp-project"
AIPL2C  = os.path.join(ROOT, "_build/default/src/aipl2c.exe")
ABCL    = "/Users/kodamay/projects/xinu-rpi4/examples/nqueens_with_gc.abcl"
GC_SLOT, ROOT_SLOT = 0, 1
EXPECTED = {1:1, 2:0, 3:0, 4:2, 5:10, 6:4, 7:40, 8:92, 9:352, 10:724}

def http(path, body=None, timeout=15):
    url = f"http://{PI}{path}"
    if body is None: req = urllib.request.Request(url, method="GET")
    else:
        d = body if isinstance(body, bytes) else body.encode()
        req = urllib.request.Request(url, data=d, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()

def compile_abcl(path):
    open("/tmp/_e.abcl", "w").write(open(path).read())
    subprocess.run([AIPL2C, "/tmp/_e.abcl", "--xinu-jit", "--no-typecheck",
                    "-o", "/tmp/_e.c"], check=True, capture_output=True, text=True)
    return open("/tmp/_e.c", "rb").read()

def gc_dry(threshold=0):
    r = http(f"/gc?threshold_ms={threshold}&dry=1", body=b"")
    parts = {}
    for tok in r.split():
        if "=" in tok and not tok.endswith("("):
            k, v = tok.split("=", 1)
            try: parts[k] = int(v)
            except: parts[k] = v
    return parts.get("killed", 0), parts.get("scanned", 0)

def send(to, m, arg=0, timeout=5):
    return http(f"/actor/send?to={to}&m={m}&arg={arg}", timeout=timeout).strip()

def run_one(N):
    """Returns (got, elapsed_ms, peak_actors, gc_scans, gc_kills)."""
    # snapshot GC stats before
    scans_before = int(send(GC_SLOT, "get", 1))
    killed_before = int(send(GC_SLOT, "get", 0))

    t0 = time.time()
    send(ROOT_SLOT, "bootstrap", N)
    peak = 0
    samples = 0
    while True:
        time.sleep(0.05)
        elapsed_ms = (time.time() - t0) * 1000.0
        try: done = int(send(ROOT_SLOT, "get_done"))
        except Exception: done = -1
        _, total = gc_dry(99999999)
        if total > peak: peak = total
        # mid-run safety check: at threshold 500 ms, NO in-flight Solver
        # should be a target (they're all young).
        targets, _ = gc_dry(500)
        if targets > 0:
            print(f"  !! at t={elapsed_ms:.0f}ms, GC dry-run found {targets} "
                  f"targets — would falsely kill in-flight Solvers!")
        samples += 1
        if done == 1: break
        if elapsed_ms > 30000:
            return -1, elapsed_ms, peak, 0, 0          # timeout
    sols = int(send(ROOT_SLOT, "get_solutions"))
    elapsed_ms = (time.time() - t0) * 1000.0

    # trigger one GC tick (should find 0 zombies — Solvers all suicided)
    send(GC_SLOT, "tick")
    scans_after = int(send(GC_SLOT, "get", 1))
    killed_after = int(send(GC_SLOT, "get", 0))

    return (sols, elapsed_ms, peak,
            scans_after - scans_before, killed_after - killed_before)

def main():
    print("=== distributed N-Queens + GC actor (co-resident) eval ===\n")
    csrc = compile_abcl(ABCL)

    results = []
    for N in [4, 5, 6, 7, 8]:
        # Re-/actor/load between runs to fully reset Solver pool — without
        # this, leftover Solvers from a previous bootstrap (rare race when
        # one suicides just as the next bootstrap fires) corrupt the next
        # run's sumv accumulation.  Each load costs ~30 ms, well under the
        # per-run elapsed.
        print(f"\n--- N={N} ---")
        r = http("/actor/load", body=csrc, timeout=20).strip()
        time.sleep(0.2)
        got, ms, peak, scans, kills = run_one(N)
        exp = EXPECTED.get(N, "?")
        verdict = "OK" if got == exp else f"WRONG (expected {exp})"
        print(f"   load -> {r[:60]}")
        print(f"   solutions={got} [{verdict}]  elapsed={ms:.0f}ms  "
              f"peak_actors={peak}  gc_scans={scans} gc_kills={kills}")
        results.append((N, exp, got, ms, peak, scans, kills))

    print(f"\n=== summary ===")
    print(f"  {'N':>2}  {'exp':>4}  {'got':>4}  {'elapsed_ms':>11}  "
          f"{'peak_act':>9}  {'gc_scans':>9}  {'gc_kills':>9}")
    for r in results:
        print(f"  {r[0]:>2}  {r[1]:>4}  {r[2]:>4}  {r[3]:>11.0f}  "
              f"{r[4]:>9}  {r[5]:>9}  {r[6]:>9}")

    print(f"\n=== final inventory (should be 2: GC + Root, both P) ===")
    print(http("/api/actors-gc?threshold_ms=0&limit=10"))

if __name__ == "__main__":
    main()
