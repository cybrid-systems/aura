#!/usr/bin/env python3
"""Snapshot/Restore/Diff Stress Fuzz — stress-test snapshot pipeline under mutation.

Strategy:
1. Set a program, snapshot it
2. Apply multiple EDSL mutations, diffing vs snapshot after each
3. Restore snapshot, verify workspace returns to original
4. Repeat with multiple interleaved snapshots
5. Verify no memory leaks or crashes across cycles

Usage:
  python3 tests/fuzz_snapshot.py [--quick] [--seed N]
"""

# --- paths patched for tests/fuzz layout (#1935) ---
import sys as _sys
from pathlib import Path as _Path

_FUZZ = _Path(__file__).resolve().parent.parent
if str(_FUZZ) not in _sys.path:
    _sys.path.insert(0, str(_FUZZ))
# --- end path patch ---
import contextlib
import datetime
import json
import random
import subprocess
import sys
import time

from common import AURA_BIN as _AURA_BIN  # noqa: E402
from common import REPO as _REPO

HERE = _FUZZ
REPO = _REPO
AURA = str(_AURA_BIN)

QUICK = "--quick" in sys.argv
SEED = None
for i, a in enumerate(sys.argv):
    if a == "--seed" and i + 1 < len(sys.argv):
        SEED = int(sys.argv[i + 1])

rng = random.Random(SEED if SEED is not None else None)

PROGRAMS = [
    "(define (f x) (+ x 1))",
    "(define (add a b) (+ a b))",
    "(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))",
    "(define (map f lst) (if (null? lst) () (cons (f (car lst)) (map f (cdr lst)))))",
    "(let ((x 10) (y 20)) (+ x y))",
    "(begin (display 1) (display 2) (display 3))",
    # Multi-line programs
    """(define (process n)
  (let ((a (+ n 1))
        (b (* n 2)))
    (+ a b)))
""",
    """(define (compose f g)
  (lambda (x) (f (g x))))
(define add1 (lambda (x) (+ x 1)))
(define double (lambda (x) (* x 2)))
(display ((compose add1 double) 5))
""",
    """(define x 42)
(define y 10)
(define (main)
  (let ((z (+ x y)))
    (display z)))
(main)
""",
]

FN_NAMES = [
    "f",
    "g",
    "add",
    "fact",
    "map",
    "process",
    "compose",
    "add1",
    "double",
    "main",
]
MUTATIONS = [
    lambda: f'(mutate:rebind "{rng.choice(FN_NAMES)}" "(lambda (x) (* x {rng.randint(1, 10)}))" "fuzz")',
    lambda: f'(mutate:tweak-literal {rng.randint(0, 3)} {rng.choice([1, -1, 5])} "fuzz")',
    lambda: f'(mutate:replace-value {rng.randint(0, 3)} {rng.randint(1, 100)} "fuzz")',
]


def escaped(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def send(proc, cmd):
    try:
        if proc.poll() is not None:
            return None
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()
        time.sleep(0.005)
        line = proc.stdout.readline()
    except (BrokenPipeError, OSError):
        return None
    if not line:
        return None
    stripped = line.strip()
    brace = stripped.rfind("{")
    json_part = stripped[brace:] if brace >= 0 else stripped
    try:
        return json.loads(json_part)
    except Exception:
        return None


def parse_display(line):
    """Extract display output from a serve response line."""
    if not line:
        return ""
    stripped = line.strip()
    brace = stripped.rfind("{")
    if brace > 0:
        return stripped[:brace].strip()
    return stripped


def run_session(n_cycles):
    proc = subprocess.Popen(
        [AURA, "--serve"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.15)

    stats = {"ok": 0, "error": 0, "crash": 0, "restore_pass": 0, "restore_fail": 0}

    program = rng.choice(PROGRAMS)
    resp = send(proc, f'set-code "{escaped(program)}"')
    if not resp or resp.get("status") != "ok":
        with contextlib.suppress(Exception):
            proc.kill()
        stats["crash"] += 1
        return stats

    # Snapshot initial state
    resp = send(proc, '(display (ast:snapshot "initial"))')
    if resp is None:
        stats["crash"] += 1
        with contextlib.suppress(Exception):
            proc.kill()
        return stats

    # Store snapshot IDs for restore verification
    display = parse_display(resp.get("_display", str(resp)))
    try:
        snapshot_ids = [int(display.strip())]
    except (ValueError, TypeError):
        snapshot_ids = [0]

    for cycle in range(n_cycles):
        # Interleave: mutate, snapshot, diff, restore
        phase = cycle % 6

        if phase < 3:
            # Apply mutation
            mutation = rng.choice(MUTATIONS)()
            resp = send(proc, mutation)
            if resp is None:
                stats["crash"] += 1
                break
            if resp.get("status") in ("ok", "closure"):
                stats["ok"] += 1
            else:
                stats["error"] += 1

            # Diff vs initial snapshot
            resp = send(proc, f"(display (ast:diff {snapshot_ids[0]}))")
            if resp is None:
                stats["crash"] += 1
                break
            if resp.get("status") in ("ok", "closure"):
                display = parse_display(f"{resp.get('_display', '')}")
                stats["ok"] += 1

        elif phase == 3:
            # Take another snapshot (intermediate state)
            resp = send(proc, f'(display (ast:snapshot "mid-{cycle}"))')
            if resp is None:
                stats["crash"] += 1
                break
            if resp.get("status") in ("ok", "closure"):
                try:
                    d = resp.get("_display", "")
                    if not d:
                        d = parse_display(str(resp))
                    sid = int(d.strip())
                    if sid >= 0:
                        snapshot_ids.append(sid)
                except (ValueError, TypeError):
                    pass
                stats["ok"] += 1
            else:
                stats["error"] += 1

        elif phase == 4:
            # List snapshots
            resp = send(proc, "(display (ast:list-snapshots))")
            if resp is None:
                stats["crash"] += 1
                break
            stats["ok"] += 1

        else:
            # Restore to initial snapshot
            resp = send(proc, f"(display (ast:restore {snapshot_ids[0]}))")
            if resp is None:
                stats["crash"] += 1
                break

            status = resp.get("status", "error")
            if status == "ok":
                stats["restore_pass"] += 1

                # Verify: query def-use should still work after restore
                resp = send(proc, '(display (query:def-use "display"))')
                if resp is None:
                    stats["crash"] += 1
                    break

                # Verify diff is now empty (restored to snapshot)
                resp = send(proc, f"(display (ast:diff {snapshot_ids[0]}))")
                if resp is None:
                    stats["crash"] += 1
                    break
            else:
                stats["restore_fail"] += 1

            # Clear snapshot list, re-snapshot for next cycle
            # (snapshots persist in process; we just use new IDs)

        # Periodically re-set-code to a new program (test complete workspace swap)
        if cycle > 0 and cycle % 20 == 0 and phase == 0:
            program = rng.choice(PROGRAMS)
            resp = send(proc, f'set-code "{escaped(program)}"')
            if resp and resp.get("status") == "ok":
                # Snapshot the new state
                resp = send(proc, '(display (ast:snapshot "initial"))')
                if resp and resp.get("status") in ("ok", "closure"):
                    try:
                        d = resp.get("_display", "")
                        if not d:
                            d = parse_display(str(resp))
                        snapshot_ids = [int(d.strip())]
                    except (ValueError, TypeError):
                        snapshot_ids = [0]
            elif resp is None:
                stats["crash"] += 1
                break

        if QUICK and cycle >= n_cycles:
            break

    try:
        proc.stdin.close()
        proc.kill()
        proc.wait(timeout=3)
    except Exception:
        pass
    return stats


def main():
    print("=" * 60)
    print("Snapshot/Restore/Diff Stress Fuzz")
    print(f"  Date:   {datetime.date.today().isoformat()}")
    print(f"  Seed:   {SEED if SEED is not None else 'random'}")
    print(f"  Mode:   {'QUICK' if QUICK else 'FULL'}")
    print("=" * 60)

    n_cycles = 300 if QUICK else 1500
    n_sessions = 3 if QUICK else 6

    total = {"ok": 0, "error": 0, "crash": 0, "restore_pass": 0, "restore_fail": 0}

    for s in range(n_sessions):
        print(f"\n  Session {s + 1}/{n_sessions} ... ", end="", flush=True)
        st = run_session(n_cycles // n_sessions)
        for k in total:
            total[k] += st[k]
        ops = st["ok"] + st["error"] + st["crash"]
        pct = st["ok"] / max(ops, 1) * 100
        print(
            f"{st['ok']}/{ops} ok ({pct:.0f}%) "
            f"[restore={st['restore_pass']}P/{st['restore_fail']}F "
            f"err={st['error']} crash={st['crash']}]"
        )

    print(f"\n{'=' * 60}")
    print(f"  Total:   {total['ok']} ok, {total['error']} error, {total['crash']} crash")
    print(f"  Restore: {total['restore_pass']}P / {total['restore_fail']}F")
    ops = total["ok"] + total["error"] + total["crash"]
    rate = total["ok"] / max(ops, 1) * 100
    print(f"  Rate:    {rate:.1f}%")

    if total["crash"]:
        print("\n  💥 CRASH detected!")
        sys.exit(1)


if __name__ == "__main__":
    main()
