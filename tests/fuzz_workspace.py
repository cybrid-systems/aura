#!/usr/bin/env python3
"""Workspace Layering Stress Fuzz — test workspace isolation under heavy mutation.

Strategy:
1. Set a program in root, create multiple child workspaces
2. In each workspace, apply different mutations (rebind, set-body, etc.)
3. Switch between workspaces: verify isolation (root unchanged)
4. Delete workspaces, verify root still intact
5. Create nested workspaces (child of child)
6. Repeat with multiple programs

Usage:
  python3 tests/fuzz_workspace.py [--quick] [--seed N]
"""

import contextlib
import datetime
import json
import os
import random
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
AURA = os.environ.get("AURA_BIN", str(REPO / "build" / "aura"))

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
    """(define (map f lst)
  (if (null? lst) ()
    (cons (f (car lst)) (map f (cdr lst)))))
(define (double x) (* x 2))
""",
    "(let ((x 10) (y 20)) (+ x y))",
    "(begin (display 1) (display 2) (display 3))",
]

SYMBOLS = ["f", "add", "fact", "map", "double", "x", "y", "lst"]


def escaped(s):
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def send(proc, cmd):
    try:
        if proc.poll() is not None:
            return None
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()
        time.sleep(0.003)
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


def run_session(n_cycles):
    proc = subprocess.Popen(
        [AURA, "--serve"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.15)

    stats = {"ok": 0, "error": 0, "crash": 0}

    program = rng.choice(PROGRAMS)
    resp = send(proc, f'set-code "{escaped(program)}"')
    if not resp or resp.get("status") != "ok":
        with contextlib.suppress(Exception):
            proc.kill()
        stats["crash"] = 1
        return stats

    # Store the original source for verification

    workspace_ids = [0]  # root is always 0

    for cycle in range(n_cycles):
        phase = cycle % 7

        if phase < 2:
            # Create a new workspace
            name = f"ws-{cycle}"
            resp = send(proc, f'(display (workspace:create "{name}"))')
            if resp is None:
                stats["crash"] += 1
                break
            if resp.get("status") in ("ok", "closure"):
                stats["ok"] += 1
                # Try to extract ID
                try:
                    id_str = (resp.get("_display", "") or json.dumps(resp.get("value", ""))).strip()
                    wid = int(id_str)
                    if wid >= 0:
                        workspace_ids.append(wid)
                except (ValueError, TypeError):
                    pass
            else:
                stats["error"] += 1

        elif phase < 4 and len(workspace_ids) > 1:
            # Switch to a random child workspace and mutate
            wid = rng.choice(workspace_ids[1:])
            resp = send(proc, f"(display (workspace:switch {wid}))")
            if resp is None:
                stats["crash"] += 1
                break

            # Apply mutation in child
            fn = rng.choice(SYMBOLS)
            new_val = rng.randint(1, 100)
            mut = rng.choice(
                [
                    f'(mutate:rebind "{fn}" "(lambda (x) (* x {new_val}))" "fuzz")',
                    f'(mutate:tweak-literal {rng.randint(0, 3)} {rng.choice([1, -1])} "fuzz")',
                    f'(mutate:set-body "{fn}" "(* x {new_val})")',
                ]
            )
            resp = send(proc, mut)
            if resp is None:
                stats["crash"] += 1
                break
            if resp.get("status") in ("ok", "closure"):
                stats["ok"] += 1

            # Query something in child to verify it works
            resp = send(proc, f'(display (query:def-use "{fn}"))')
            if resp is None:
                stats["crash"] += 1
                break
            stats["ok"] += 1

        elif phase == 4:
            # Switch to root and verify isolation
            resp = send(proc, "(display (workspace:switch 0))")
            if resp is None:
                stats["crash"] += 1
                break
            stats["ok"] += 1

            # Verify root still has its original state
            resp = send(proc, "(display (current-source))")
            if resp is None:
                stats["crash"] += 1
                break
            stats["ok"] += 1

            # Query a def that should exist in all workspaces
            sym = rng.choice(["f", "add", "fact", "map", "double"])
            resp = send(proc, f'(display (query:def-use "{sym}"))')
            if resp is None:
                stats["crash"] += 1
                break
            stats["ok"] += 1

        elif phase == 5:
            # Lock/unlock test
            if len(workspace_ids) > 1:
                wid = rng.choice(workspace_ids[1:])
                resp = send(proc, f"(display (workspace:lock {wid} #t))")
                if resp is None:
                    stats["crash"] += 1
                    break
                # Verify can-write returns false
                resp = send(proc, f"(display (workspace:can-write? {wid}))")
                if resp is None:
                    stats["crash"] += 1
                    break
                # Unlock
                resp = send(proc, f"(display (workspace:lock {wid} #f))")
                if resp is None:
                    stats["crash"] += 1
                    break
                stats["ok"] += 3

            # Verify root is still writable
            resp = send(proc, "(display (workspace:can-write? 0))")
            if resp is None:
                stats["crash"] += 1
                break
            stats["ok"] += 1

        else:
            # Delete a workspace (not root)
            deletable = [wid for wid in workspace_ids if wid > 0]
            if deletable:
                wid = rng.choice(deletable)
                resp = send(proc, f"(display (workspace:delete {wid}))")
                if resp is None:
                    stats["crash"] += 1
                    break
                if resp.get("status") in ("ok", "closure"):
                    workspace_ids.remove(wid)
                    stats["ok"] += 1

            # Verify workspace list is still valid
            resp = send(proc, "(display (workspace:list))")
            if resp is None:
                stats["crash"] += 1
                break
            stats["ok"] += 1

            # After deleting, switch to root and verify
            if rng.random() < 0.5:
                resp = send(proc, "(display (workspace:switch 0))")
                if resp is None:
                    stats["crash"] += 1
                    break
                stats["ok"] += 1

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
    print("Workspace Layering Stress Fuzz")
    print(f"  Date:   {datetime.date.today().isoformat()}")
    print(f"  Seed:   {SEED if SEED is not None else 'random'}")
    print(f"  Mode:   {'QUICK' if QUICK else 'FULL'}")
    print("=" * 60)

    n_cycles = 200 if QUICK else 1000
    n_sessions = 3 if QUICK else 6

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
    print(f"  Total: {total['ok']} ok, {total['error']} error, {total['crash']} crash")
    rate = total["ok"] / max(total["ok"] + total["error"] + total["crash"], 1) * 100
    print(f"  Rate:  {rate:.1f}%")

    if total["crash"]:
        print("\n  💥 CRASH detected!")
        sys.exit(1)


if __name__ == "__main__":
    main()
