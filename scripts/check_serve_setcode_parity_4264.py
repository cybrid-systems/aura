#!/usr/bin/env python3
"""Issue #4264: Soft set-code + eval-current must produce the same CASE stdout as oneshot.

Soft --serve-async set-code + eval-current diverged from oneshot for the SAME
source (the soft_score_inflate → verify_no_gain family) through two silent
faces:

  FACE A (prelude): oneshot (aura file.aura / pipe / --load) seeds the Soft
  std prelude (#4178–#4219: require std/list, std/string, std/hash, std/math
  all:) plus the #4228 sync_soft_export_cells_for_ir() TopCellLoad sync before
  the program text. Serve-async session services never did, so a set-code'd
  source using any require-injected std binding evaluated to "unbound
  variable: make-hash" under (eval-current) while the same source oneshot
  ran — the host scored a truncated/empty CASE display.

  FACE B (EDEADLK): a (require …) INSIDE the set-code'd source re-entered
  load_module_file's fresh unique_lock(workspace_mtx_) while (eval-current)
  held the workspace unique via WorkspaceUniqueIfNeeded (lock_order
  depth-stamped per #4128) — std::shared_mutex is non-recursive, so the
  same-thread re-lock returned EDEADLK and the serve exec surfaced "exec
  exception: Resource deadlock avoided" while the same source oneshot
  required cleanly.

Fix contract:
  AC1 loader adopt-if-held: WorkspaceAdoptIfNeeded probes
      lock_order::is_held(Level::Workspace); all 8 fresh re-lock sites in
      load_module_file adopt the already-held exclusive instead (zero old
      fresh-lock literals remain in the loader).
  AC2 serve prelude parity: load_soft_session_prelude cites #4264, carries
      all four oneshot require lines, and runs the #4228 cell sync.
  AC3 prelude wiring: helper runs at default + own-service named + bench
      session creation (definition + 3 call sites = 4 occurrences).
  AC4 runtime doors: tests/serve/test_concurrent.cpp
      test_issue_4264_session_prelude_parity (AC1/AC2/AC3 source-cite rows)
      + tests/core/test_workspace_lock_reentrancy.cpp AC6 (require inside
      set-code'd source + eval-current completes, no EDEADLK).
  AC5 build.py wiring + root_check_allowlist entry; no docs/design/4264-*.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text() if p.exists() else ""


def main() -> int:
    failures: list[str] = []

    def check(cond: bool, label: str) -> None:
        if not cond:
            failures.append(label)

    # ── AC1: loader adopt-if-held contract ────────────────────────────
    loader = _read("src/compiler/evaluator_module_loader.cpp")
    check(
        "WorkspaceAdoptIfNeeded" in loader, "AC1: WorkspaceAdoptIfNeeded helper present in evaluator_module_loader.cpp"
    )
    check("Issue #4264" in loader, "AC1: loader cites #4264 next to the adopt helper")
    check(
        "lock_order::is_held(lock_order::Level::Workspace)" in loader,
        "AC1: adopt probe consults lock_order::is_held(Level::Workspace)",
    )
    adopt_sites = loader.count("WorkspaceAdoptIfNeeded wlock_adopt_4264(workspace_mtx_);")
    check(adopt_sites == 8, f"AC1: all 8 load-path lock sites adopt the held exclusive (found {adopt_sites})")
    old_locks = loader.count("std::unique_lock<std::shared_mutex> wlock(workspace_mtx_);")
    check(old_locks == 0, f"AC1: fresh workspace re-lock removed from the loader (found {old_locks})")

    # ── AC2: serve prelude parity helper ──────────────────────────────
    serve = _read("src/serve/serve_async.cpp")
    check(
        "static void load_soft_session_prelude(aura::compiler::CompilerService& cs)" in serve,
        "AC2: load_soft_session_prelude helper present in serve_async.cpp",
    )
    check(
        "Issue #4264: Soft serve-async sessions must bind the same Soft std prelude" in serve,
        "AC2: prelude helper cites #4264",
    )
    check(
        "#4178" in serve and "#4228" in serve,
        "AC2: helper cites the oneshot prelude (#4178) and cell-sync (#4228) contracts",
    )
    for mod in ("std/list", "std/string", "std/hash", "std/math"):
        needle = '(require \\"' + mod + '\\" all:)'
        check(needle in serve, f"AC2: prelude requires {mod} all: (oneshot prelude line)")
    sync_pos = serve.find("static void load_soft_session_prelude")
    sync_end = serve.find("run_serve_async", sync_pos) if sync_pos != -1 else -1
    check(
        sync_pos != -1 and sync_end != -1 and "sync_soft_export_cells_for_ir" in serve[sync_pos:sync_end],
        "AC2: helper runs the #4228 sync_soft_export_cells_for_ir",
    )

    # ── AC3: prelude wiring at session creation ───────────────────────
    calls = serve.count("load_soft_session_prelude(")
    check(
        calls == 4,
        f"AC3: prelude wired at default + own-service named + bench (definition + 3 call sites = 4, found {calls})",
    )

    # ── AC4: runtime doors ────────────────────────────────────────────
    tc = _read("tests/serve/test_concurrent.cpp")
    check(
        "test_issue_4264_session_prelude_parity" in tc,
        "AC4: test_concurrent hosts test_issue_4264_session_prelude_parity",
    )
    check(
        'run_test("test_issue_4264_session_prelude_parity"' in tc,
        "AC4: 4264 parity test registered in test_concurrent main()",
    )
    for row in ("#4264 AC1", "#4264 AC2", "#4264 AC3"):
        check(row in tc, f"AC4: test_concurrent carries the {row} row")
    reent = _read("tests/core/test_workspace_lock_reentrancy.cpp")
    check(
        "AC6 (#4264)" in reent, "AC4: test_workspace_lock_reentrancy AC6 covers require inside set-code + eval-current"
    )
    check(
        "eval-current completes require + hash program" in reent,
        "AC4: AC6 runtime row asserts the require + hash program completes",
    )

    # ── AC5: build wiring + allowlist + no docs/design ────────────────
    build_py = _read("build.py")
    check(
        "check_serve_setcode_parity_4264.py" in build_py,
        "AC5: build.py cmd_lint wires check_serve_setcode_parity_4264.py",
    )
    allow = _read("scripts/coverage/root_check_allowlist.txt")
    check("check_serve_setcode_parity_4264.py" in allow, "AC5: root_check_allowlist.txt carries the linter entry")
    docs = ROOT / "docs" / "design"
    if docs.exists():
        stray = [p.name for p in docs.glob("4264-*")]
        check(not stray, f"AC5: no docs/design/4264-* files (found {stray})")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"check_serve_setcode_parity_4264: {len(failures)} row(s) failed", file=sys.stderr)
        return 1
    print("check_serve_setcode_parity_4264: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
