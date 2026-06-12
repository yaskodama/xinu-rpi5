#!/usr/bin/env python3
"""bench_nqueens.py — N-Queens load-distribution benchmark.

Three configurations on the same algorithm:

  1. Mac alone   — AIPL on the Py·I runtime, single thread (nqueens_subset.abcl)
  2. Xinu alone  — bare-metal Pi 4, native C via /compile (one HTTP call)
  3. Mac + Xinu  — first-row columns split between Mac AIPL and Xinu,
                   run concurrently in two threads

All variants compute identical NQueens (counts solutions; result must
match the known value).  Splits are made at row 0 — N independent
sub-trees, each "queen-at-col-K" → count completions.

Output: a Markdown table on stdout (and appended to RESULTS.md with
--save).
"""

import argparse, concurrent.futures, os, re, shutil, subprocess, sys, tempfile, time, urllib.request

HERE       = os.path.dirname(os.path.abspath(__file__))
ABCL_TPL   = os.path.join(HERE, "nqueens_subset.abcl")
REPO_ROOT  = "/Users/kodamay/ocaml-app/abclcp-project"
AIPL_MAIN  = os.path.join(REPO_ROOT, "src/python-aipl/aipl_main.py")
XINU       = os.environ.get("XINU_URL", "http://192.168.3.100/compile")

C_SUBTREE_RANGE = r'''
int cols[12];
int safe(int row, int c) {
  int i = 0;
  while (i < row) {
    int p = cols[i];
    if (p == c) return 0;
    if (p - c == row - i) return 0;
    if (c - p == row - i) return 0;
    i = i + 1;
  }
  return 1;
}
int count(int n, int row) {
  if (row == n) return 1;
  int total = 0;
  int c = 0;
  while (c < n) {
    if (safe(row, c)) {
      cols[row] = c;
      total = total + count(n, row + 1);
    }
    c = c + 1;
  }
  return total;
}
int main() {
  int total = 0;
  int k = %d;
  while (k < %d) {
    cols[0] = k;
    total = total + count(%d, 1);
    k = k + 1;
  }
  return total;
}
'''


def xinu_range(n: int, k_first: int, k_last: int) -> tuple[int, float]:
    """Per-partition /cc calls (one HTTP call per row-0 column k) so each stays
    within the Pi5 cc-JIT 250ms runaway deadline."""
    if k_first >= k_last:
        return 0, 0.0
    total = 0
    elapsed = 0.0
    for k in range(k_first, k_last):
        src = (C_SUBTREE_RANGE % (k, k + 1, n)).encode()
        req = urllib.request.Request(XINU, data=src, method="POST")
        start = time.perf_counter()
        with urllib.request.urlopen(req, timeout=300) as r:
            body = r.read().decode()
        elapsed += time.perf_counter() - start
        got = None
        for line in body.splitlines():
            if line.startswith("=> "):
                got = int(line[3:].strip()); break
        if got is None:
            raise RuntimeError("no '=> N' from xinu: " + body[:200])
        total += got
    return total, elapsed


_PARAM_RE = re.compile(r"^var\s+(N|K_FIRST|K_LAST)\s*=\s*[0-9]+\s*;", re.M)


def mac_range(n: int, k_first: int, k_last: int) -> tuple[int, float]:
    """Rewrite the three `var N=…; var K_FIRST=…; var K_LAST=…;` lines at
    the top of a private copy of nqueens_subset.abcl, then run it once."""
    if k_first >= k_last:
        return 0, 0.0
    src = open(ABCL_TPL).read()

    def repl(m):
        v = {"N": n, "K_FIRST": k_first, "K_LAST": k_last}[m.group(1)]
        return f"var {m.group(1)} = {v};"

    patched = _PARAM_RE.sub(repl, src)
    with tempfile.NamedTemporaryFile("w", suffix=".abcl", delete=False) as f:
        f.write(patched)
        tmp = f.name
    try:
        start = time.perf_counter()
        out = subprocess.check_output([sys.executable, AIPL_MAIN, tmp],
                                       cwd=REPO_ROOT, text=True, timeout=600)
        elapsed = time.perf_counter() - start
    finally:
        os.unlink(tmp)
    for line in out.splitlines():
        if line.startswith("nqueens_subset"):
            m = re.search(r"subtotal=(\d+)", line)
            if m:
                return int(m.group(1)), elapsed
    raise RuntimeError(f"no 'subtotal=' in aipl output:\n{out[-400:]}")


def distributed(n: int, mac_share: int) -> tuple[int, float, float, float]:
    """Run mac_range and xinu_range in parallel; report total + the two
    individual times so the split-balance is visible."""
    start = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
        f_mac  = ex.submit(mac_range,  n, 0, mac_share)
        f_xinu = ex.submit(xinu_range, n, mac_share, n)
        (mac_sol,  mac_t)  = f_mac.result()
        (xinu_sol, xinu_t) = f_xinu.result()
    elapsed = time.perf_counter() - start
    return mac_sol + xinu_sol, elapsed, mac_t, xinu_t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", "--N", type=int, default=10, help="board size (default 10)")
    ap.add_argument("--save", default=None, help="append summary to this file")
    args = ap.parse_args()
    n = args.N
    expected = {6: 4, 7: 40, 8: 92, 9: 352, 10: 724, 11: 2680}.get(n)

    print(f"\n=== N-Queens load distribution, N={n} ===")
    print(f"  Xinu endpoint: {XINU}")
    print()

    rows = []  # (label, solutions, total_s, mac_s, xinu_s)

    print(f"  {'Mac alone (AIPL Py·I)':30s}  ", end="", flush=True)
    sol, t = mac_range(n, 0, n)
    print(f"sol={sol:5d}  total={t:6.2f}s")
    rows.append(("Mac alone (AIPL)", sol, t, t, 0.0))

    print(f"  {'Xinu alone (cc JIT)':30s}  ", end="", flush=True)
    sol, t = xinu_range(n, 0, n)
    print(f"sol={sol:5d}  total={t:6.2f}s")
    rows.append(("Xinu alone (cc JIT)", sol, t, 0.0, t))

    # Sweep split points
    for share in (0, n // 4, n // 2, 3 * n // 4, n):
        print(f"  {f'Mac={share}+Xinu={n-share}':30s}  ", end="", flush=True)
        sol, total_t, mac_t, xinu_t = distributed(n, share)
        print(f"sol={sol:5d}  total={total_t:6.2f}s  (mac={mac_t:5.2f} xinu={xinu_t:5.2f})")
        rows.append((f"Mac={share}+Xinu={n-share}", sol, total_t, mac_t, xinu_t))

    if expected is not None:
        ok = all(r[1] == expected for r in rows)
        print(f"\n  expected sol={expected}  →  {'all correct ✓' if ok else 'MISMATCH'}")

    if args.save:
        with open(args.save, "a") as f:
            f.write(f"\n## N={n} ({time.strftime('%Y-%m-%d %H:%M:%S')})\n\n")
            f.write("| Configuration | solutions | total (s) | mac (s) | xinu (s) |\n")
            f.write("|---|---:|---:|---:|---:|\n")
            for label, sol, total_t, mac_t, xinu_t in rows:
                f.write(f"| {label} | {sol} | {total_t:.2f} | {mac_t:.2f} | {xinu_t:.2f} |\n")
            f.write("\n")
        print(f"\n  → results appended to {args.save}")


if __name__ == "__main__":
    main()
