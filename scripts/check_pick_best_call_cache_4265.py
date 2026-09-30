#!/usr/bin/env python3
"""Issue #4265: Soft pick-best eval must return a single numeric max — never the
input list, never an opaque <error> — under Soft sock stress.

VERIFICATION CONCLUSION (HEAD 985fbb8a0): the reported repro is NOT
reproducible at HEAD. Every door is clean:
  - Unit door (CompilerService::eval): repeated Path B pick-best calls with
    fresh args (10 lists incl. the WAVE10 list + its replay) return the
    correct scalar max each time; set-code + eval-current session parity
    (replay eval-current between calls) stays fresh —
    tests/compiler/test_primcall_narg.cpp AC20/AC21, green at HEAD.
  - Serve door (--serve-async, the Soft host env shape): the 16-step WAVE10
    sequence (set-code helper, eval-current, direct calls with fresh lists,
    the issue's literal let-form execs, interleaved eval-currents) returns
    the exact scalar max for every call — no list values, no <error>.
The two reported faces are explained by already-landed fixes: the <error>
face matches #4264's Face A (session std prelude now bound at all three
serve-async session-service creation sites) and the silent-alternate /
wrong-value family matches #4232 (registry prim fallback in unwired eval
envs) plus #4264's loader adopt-if-held locks. Per the ship contract ("only
fix what remains broken at HEAD"), no source change is made; this ship lands
the regression doors that pin the pick-best contract so a future regression
on this surface cannot ship silently.

Gate contract:
  AC1 doors present — test_primcall_narg.cpp cites Issue #4265 and hosts
      ac20_pick_best_pathb_repeat_calls + ac21_pick_best_session_replay_parity,
      both invoked from run_test_primcall_narg.
  AC2 WAVE10 shape pinned — the exact reported list
      (6 6 0 0 0 0 0 0 0 0 0 0 6), a fresh-args list, and the replay repeat
      of the WAVE10 list appear in AC20.
  AC3 session parity pinned — set-code + eval-current rows in AC21
      (replay between calls must not serve stale values).
  AC4 batch registration — test_ir_closure_jit_misc_batch still registers
      run_test_primcall_narg.
  AC5 build.py wiring + root_check_allowlist entry; no docs/design/4265-*.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.exists() and (ROOT / rel).read_text() or ""


def main() -> int:
    failures: list[str] = []

    def check(cond: bool, label: str) -> None:
        if not cond:
            failures.append(label)

    # ── AC1: doors present and invoked ────────────────────────────────
    tc = _read("tests/compiler/test_primcall_narg.cpp")
    check("#4265" in tc, "AC1: test file cites #4265")
    check(
        "ac20_pick_best_pathb_repeat_calls" in tc,
        "AC1: AC20 repeat-call freshness door present",
    )
    check(
        "ac21_pick_best_session_replay_parity" in tc,
        "AC1: AC21 session replay-parity door present",
    )
    check(
        "ac20_pick_best_pathb_repeat_calls();" in tc and "ac21_pick_best_session_replay_parity();" in tc,
        "AC1: both doors invoked from run_test_primcall_narg",
    )
    check("ac22_source_gate_4265" in tc, "AC1: AC22 source gate present")

    # ── AC2: WAVE10 repro shape pinned in AC20 ────────────────────────
    wave = "(pick-best (list 6 6 0 0 0 0 0 0 0 0 0 0 6))"
    check(
        wave in tc,
        "AC2: the exact WAVE10 list from the issue body is pinned as a case",
    )
    check(
        "(pick-best (list 0 0 9))" in tc,
        "AC2: a fresh-args list case present (fresh max must be computed)",
    )
    check(
        tc.count(wave) >= 2,
        "AC2: WAVE10 list repeated as the replay case (cache must not serve stale)",
    )

    # ── AC3: session parity rows in AC21 ──────────────────────────────
    check(
        "(set-code" in tc and "(eval-current)" in tc,
        "AC3: set-code + eval-current session shape pinned",
    )
    check(
        "post-replay pick-best still fresh" in tc,
        "AC3: post-replay freshness row present",
    )

    # ── AC4: batch registration intact ────────────────────────────────
    batch = _read("tests/compiler/test_ir_closure_jit_misc_batch.cpp")
    check(
        "run_test_primcall_narg" in batch,
        "AC4: test_ir_closure_jit_misc_batch registers run_test_primcall_narg",
    )

    # ── AC5: build wiring + allowlist + no docs/design ────────────────
    build_py = _read("build.py")
    check(
        "check_pick_best_call_cache_4265.py" in build_py,
        "AC5: build.py cmd_lint wires check_pick_best_call_cache_4265.py",
    )
    allow = _read("scripts/coverage/root_check_allowlist.txt")
    check(
        "check_pick_best_call_cache_4265.py" in allow,
        "AC5: root_check_allowlist.txt carries the linter entry",
    )
    docs = ROOT / "docs" / "design"
    if docs.exists():
        stray = [p.name for p in docs.glob("4265-*")]
        check(not stray, f"AC5: no docs/design/4265-* files (found {stray})")

    if failures:
        print("Issue #4265 pick-best contract linter FAILED:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("Issue #4265 pick-best contract linter: all ACs satisfied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
