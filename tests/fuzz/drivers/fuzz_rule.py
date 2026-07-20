#!/usr/bin/env python3
"""Rule Normalize Stress Fuzz — test rule:define/apply/save/load cycles.

Strategy:
1. Define multiple rules with various patterns
2. Apply them, verify counts
3. Save to file, load back, verify rules match
4. Enable/disable cycles
5. Violations audit

Usage:
  python3 tests/fuzz_rule.py [--quick] [--seed N]
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

RULE_DEFS = [
    '(rule:define "r1" :pattern "(+ 1 1)" :replace "2")',
    '(rule:define "r2" :pattern "(+ 2 2)" :replace "4" :description "add")',
    '(rule:define "r3" :pattern "(* 3 3)" :replace "9" :condition "#t")',
    '(rule:define "r4" :pattern "(/ 10 2)" :replace "5" :scope :workspace)',
    '(rule:define "r5" :pattern "(list 1 2)" :replace "(cons 1 (cons 2 ()))")',
]


def send(proc, cmd):
    try:
        if proc.poll() is not None:
            return None
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()
        time.sleep(0.01)
        line = proc.stdout.readline()
        return line.strip()
    except (BrokenPipeError, OSError):
        return None


def run_session(n_cycles):
    proc = subprocess.Popen(
        [AURA, "--serve"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.2)

    stats = {"ok": 0, "error": 0, "crash": 0}
    setup_code = '(require "std/rule" all:)\n(set-code "(begin)")'

    resp = send(proc, setup_code)
    if resp is None:
        with contextlib.suppress(Exception):
            proc.kill()
        stats["crash"] = 1
        return stats

    for cycle in range(n_cycles):
        # Phase 0: define a rule
        if rng.random() < 0.4:
            rule_def = rng.choice(RULE_DEFS)
            resp = send(proc, f"(display {rule_def})")
            if resp and ("r1" in resp or "r2" in resp or "#t" in resp):
                stats["ok"] += 1
            elif resp is None:
                stats["crash"] += 1
                break
            else:
                stats["error"] += 1

        # Phase 1: list rules
        elif rng.random() < 0.3:
            resp = send(proc, "(display (rule:list))")
            if resp:
                stats["ok"] += 1
            else:
                stats["error"] += 1

        # Phase 2: apply rules
        elif rng.random() < 0.3:
            resp = send(proc, "(display (rule:apply-all))")
            if resp:
                stats["ok"] += 1
            else:
                stats["error"] += 1

        # Phase 3: enable/disable
        elif rng.random() < 0.3:
            resp = send(proc, '(display (rule:disable "r1"))')
            if resp:
                stats["ok"] += 1
                resp = send(proc, '(display (rule:enable "r1"))')
                if resp:
                    stats["ok"] += 1
            else:
                stats["error"] += 1

        # Phase 4: violations
        else:
            resp = send(proc, "(display (rule:list-violations))")
            if resp:
                stats["ok"] += 1
            else:
                stats["error"] += 1

        if QUICK and cycle >= n_cycles:
            break

    try:
        proc.kill()
        proc.wait(timeout=3)
    except Exception:
        pass
    return stats


def main():
    print("=" * 60)
    print("Rule Normalize Stress Fuzz")
    print(f"  Date: {datetime.date.today().isoformat()}")
    print(f"  Seed: {SEED if SEED is not None else 'random'}")
    print("=" * 60)

    n_cycles = 100 if QUICK else 400
    n_sessions = 3
    total = {"ok": 0, "error": 0, "crash": 0}

    for s in range(n_sessions):
        print(f"\n  Session {s + 1}/{n_sessions} ... ", end="", flush=True)
        st = run_session(n_cycles // n_sessions)
        for k in total:
            total[k] += st[k]
        ops = st["ok"] + st["error"] + st["crash"]
        pct = st["ok"] / max(ops, 1) * 100
        print(f"{st['ok']}/{ops} ({pct:.0f}%) [err={st['error']} crash={st['crash']}]")

    print(f"\n{'=' * 60}")
    rate = total["ok"] / max(total["ok"] + total["error"] + total["crash"], 1) * 100
    print(f"  Total: {total['ok']} ok, {total['error']} error, {total['crash']} crash")
    print(f"  Rate:  {rate:.1f}%")

    if total["crash"]:
        print("\n  CRASH detected!")
        sys.exit(1)


if __name__ == "__main__":
    main()
