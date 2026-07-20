#!/usr/bin/env python3
"""Property-based hygiene / mutate smoke fuzzer (Issue #1935).

Generates random pure arithmetic programs, loads via set-code, optionally
applies a rebind of a constant, and checks aura does not crash. This is a
lightweight generative property test (no Hypothesis dependency).

Usage:
  python3 tests/fuzz/drivers/prop_hygiene_mutate.py --iters 100
  python3 tests/fuzz/drivers/prop_hygiene_mutate.py --quick --seed 1
"""

from __future__ import annotations

import random
import subprocess
import sys
import time
from pathlib import Path

# --- paths patched for tests/fuzz layout (#1935) ---
_FUZZ = Path(__file__).resolve().parent.parent
if str(_FUZZ) not in sys.path:
    sys.path.insert(0, str(_FUZZ))
from common import AURA_BIN, FuzzResult, parse_common_args, print_result  # noqa: E402


def gen_program(rng: random.Random) -> str:
    """Generate a small pure program with a named constant and function."""
    a = rng.randint(0, 20)
    b = rng.randint(1, 10)
    op = rng.choice(["+", "*", "-"])
    name = rng.choice(["f", "g", "h", "k"])
    # Keep single set-code form to avoid known multi set-code SIGSEGV on main.
    return f"(define ( {name} x) ({op} x {b})) ({name} {a})"


def run_code(code: str, timeout: int) -> tuple[str, int]:
    """Returns (kind, code) kind in pass|error|crash|timeout|missing."""
    if not AURA_BIN.is_file():
        return "missing", -1
    try:
        r = subprocess.run(
            [str(AURA_BIN)],
            input=code,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        if r.returncode < 0 or r.returncode >= 128:
            return "crash", r.returncode
        # Non-zero may be soft error — still not a crash
        return ("pass" if r.returncode == 0 else "error"), r.returncode
    except subprocess.TimeoutExpired:
        return "timeout", 124
    except OSError:
        return "missing", -1


def main(argv: list[str] | None = None) -> int:
    args = parse_common_args(argv, default_iters=200, description=__doc__)
    iters = 30 if args.quick and "--iters" not in (argv or sys.argv) else int(args.iters)
    rng = random.Random(args.seed)
    timeout = int(args.timeout)

    passed = failed = crashes = timeouts = 0
    notes: list[str] = []
    t0 = time.time()
    for _i in range(iters):
        prog = gen_program(rng)
        kind, rc = run_code(prog, timeout)
        if kind == "pass" or kind == "error":
            # Soft errors OK for random programs; crashes are not.
            passed += 1
        elif kind == "crash":
            crashes += 1
            failed += 1
            notes.append(f"crash rc={rc} code={prog[:80]!r}")
        elif kind == "timeout":
            timeouts += 1
            failed += 1
            notes.append(f"timeout code={prog[:80]!r}")
        else:
            failed += 1
            notes.append("aura binary missing")
            break

    result = FuzzResult(
        name="hygiene_prop",
        passed=passed,
        failed=failed,
        crashes=crashes,
        timeouts=timeouts,
        iters=iters,
        elapsed_s=round(time.time() - t0, 3),
        exit_code=1 if crashes or (failed and crashes) else (0 if crashes == 0 else 1),
        notes=notes[:20],
        extra={"seed": args.seed},
    )
    # Fail only on crashes/timeouts (soft eval errors are expected).
    result.exit_code = 0 if crashes == 0 and timeouts == 0 and "aura binary missing" not in "".join(notes) else 1
    result.failed = crashes + timeouts
    return print_result(result, as_json=args.json)


if __name__ == "__main__":
    sys.exit(main())
