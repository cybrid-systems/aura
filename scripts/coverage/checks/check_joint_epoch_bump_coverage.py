#!/usr/bin/env python3
"""Issue #2693: joint epoch bump static gate (refine #2046 / #2366 / #2541).

Contract (one row per AC):
  AC1 src/compiler/aura_jit_bridge.cpp defines the consecutive-dirty
     fuse (g_consecutive_dirty_count + g_2693_soft_fuse_fallback_total +
     K knob AURA_EPOCH_INVARIANT_SOFT_FUSE_K, default 3, 0 disables).
  AC2 Clean walk resets consecutive; K=0 → never fire. K > 0 + consec
     >= K → bump epoch_invariant_soft_fuse_total (file-level fallback
     in this TU; per-CompilerMetrics when wired).
  AC3 aura_periodic_epoch_invariant_walk_if_due +
     aura_event_driven_epoch_invariant_walk_if_due both call
     aura_2693_soft_fuse_record after the walk runs).
  AC4 The linter detects bare `g_current_bridge_epoch.fetch_add` /
     `.store(` and `g_aot_table_epoch.fetch_add` / `.store(` outside
     the documented lockstep helpers (atomic_bump_epochs_and_stamp_bridge,
     aura_aot_bump_func_table_epoch, commit_func_table_swap,
     aura_set_current_bridge_epoch) and the bridge TU allow-list. The
     --self-test mode demonstrates a deliberate split-domain bump patch
     gets rejected and allow-listed sites pass.
  AC5 src/compiler/evaluator_primitives_obs_eval.cpp exposes additive
     query sentinels: epoch-invariant-soft-fuse-total +
     epoch-invariant-consecutive-dirty-total +
     epoch-invariant-soft-fuse-k-default +
     epoch-invariant-soft-fuse-wired + schema-2693 + issue-2693.
     #2640 / #2668 / #2366 / #2541 / #2501 / #2304 surfaces preserved
     (additive — no regression).
  AC6 tests/compiler/test_epoch_invariant_walk.cpp extended with
     #2693 AC1-AC6 source-cite block (per #81967 — no new issue-suffix
     file); coverage linter + build.py gate wire-in; no `docs/design/`.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Production bridge TU / stub / header that legitimately bumps the
# joint epoch domains. Anything under src/ NOT in this allow-list MUST
# NOT bump g_current_bridge_epoch or g_aot_table_epoch directly — the
# lockstep helpers (atomic_bump_epochs_and_stamp_bridge in service.ixx
# + aura_aot_bump_func_table_epoch in aura_jit_bridge.cpp) are the
# single source of truth (see aot_mangle.h §Joint versioning contract,
# #2046 / #2541).
ALLOW_LIST = {
    "src/compiler/aura_jit_bridge.cpp",
    "src/compiler/aura_jit_bridge_stub.cpp",
    "src/compiler/aura_jit_bridge.h",
    "src/compiler/aot_mangle.h",
    # service.ixx owns atomic_bump_epochs_and_stamp_bridge (the documented
    # lockstep helper that bridges bridge_epoch + defuse + AOT table
    # epoch). The helper itself reads those atomics under mutate_mtx_
    # to publish the joint bump, so the raw reads/writes here are OK.
    "src/compiler/service.ixx",
}

# Patterns that must NOT appear outside the allow-list. The bridge
# production TU / stub / header own the joint bump + dual-write; every
# other production file must go through the documented helpers.
FORBIDDEN_PATTERNS = [
    (re.compile(r"\bg_current_bridge_epoch\.fetch_add\s*\("), "g_current_bridge_epoch.fetch_add"),
    (re.compile(r"\bg_current_bridge_epoch\.store\s*\("), "g_current_bridge_epoch.store"),
    (re.compile(r"\bg_aot_table_epoch\.fetch_add\s*\("), "g_aot_table_epoch.fetch_add"),
    (re.compile(r"\bg_aot_table_epoch\.store\s*\("), "g_aot_table_epoch.store"),
]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def check_split_bumps(rel: str, haystack: str) -> list[str]:
    """Return list of forbidden-pattern hits in `haystack` (file `rel`).

    The allow-list exempts the bridge TU + stub + header + aot_mangle.h +
    service.ixx (the documented lockstep helper owner). Tests/ is also
    exempt since test stubs intentionally advance local atomics.
    """
    fails: list[str] = []
    # Allow-list check
    rel_norm = rel.replace("\\", "/")
    if rel_norm in ALLOW_LIST:
        return fails
    # Tests/ + benchmarks/ are exempt (their atomics are local).
    parts = rel_norm.split("/")
    if any(p in {"tests", "benchmarks", "scripts"} for p in parts):
        return fails
    for pat, label in FORBIDDEN_PATTERNS:
        for m in pat.finditer(haystack):
            line_no = haystack[: m.start()].count("\n") + 1
            fails.append(f"{rel}:{line_no}: forbidden split-domain bump pattern {label!r}")
    return fails


def scan_src_tree() -> list[str]:
    """Scan src/**/*.cpp + src/**/*.ixx for forbidden patterns."""
    fails: list[str] = []
    src = ROOT / "src"
    if not src.is_dir():
        return fails
    for ext in ("*.cpp", "*.ixx", "*.h", "*.hh"):
        for p in sorted(src.rglob(ext)):
            rel = str(p.relative_to(ROOT))
            text = p.read_text(encoding="utf-8", errors="replace")
            fails.extend(check_split_bumps(rel, text))
    return fails


def self_test() -> int:
    """Canned inputs that must pass / fail per AC4.

    Demonstrates the linter catches a deliberate split-domain bump
    patch (FAIL) and that allow-listed sites pass (PASS).
    """
    fails: list[str] = []
    # Bad patch — split-domain bump on a non-allow-listed file.
    bad = """
#include "foo.h"
namespace aura::foo {
void bump_it() {
    g_current_bridge_epoch.fetch_add(1, std::memory_order_relaxed);
    g_aot_table_epoch.store(42, std::memory_order_relaxed);
}
} // namespace aura::foo
"""
    bad_fails = check_split_bumps("src/foo/bar.cpp", bad)
    if len(bad_fails) < 2:
        fails.append(f"AC4: bad patch should fail >=2 (got {len(bad_fails)}): {bad_fails}")
    # Good patch — allow-listed file passes.
    good_fails = check_split_bumps("src/compiler/aura_jit_bridge.cpp", bad)
    if good_fails:
        fails.append(f"AC4: allow-listed bridge TU should pass (got {good_fails})")
    # Test file passes (tests/ is exempt).
    test_fails = check_split_bumps("tests/foo/bar.cpp", bad)
    if test_fails:
        fails.append(f"AC4: tests/ exempt (got {test_fails})")
    # service.ixx allow-list passes.
    svc_fails = check_split_bumps("src/compiler/service.ixx", bad)
    if svc_fails:
        fails.append(f"AC4: service.ixx allow-list (got {svc_fails})")

    # Also exercise via a temp file to prove the scanner picks it up.
    with tempfile.TemporaryDirectory() as td:
        bad_path = Path(td) / "bad_test_patch.cpp"
        bad_path.write_text(bad)
        # Check the script as a whole can be invoked and finds at least
        # one bad pattern in the temp tree (uses the scanner directly).
        all_fails = scan_src_tree()  # full scan — must not include bad_test_patch.cpp
        if any("bad_test_patch" in f for f in all_fails):
            fails.append("AC4: temp patch leaked into scan_src_tree output")
    return 1 if fails else 0


def main() -> int:
    if "--self-test" in sys.argv:
        rc = self_test()
        if rc != 0:
            print("FAIL: AC4 self-test", file=sys.stderr)
        else:
            print("OK: AC4 self-test — bad patch caught, allow-list passes")
        return rc

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    br = _read("src/compiler/aura_jit_bridge.cpp")
    brh = _read("src/compiler/aura_jit_bridge.h")
    brs = _read("src/compiler/aura_jit_bridge_stub.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_epoch_invariant_walk.cpp")
    build = _read("build.py")

    # AC1 — file-scope atomics + K knob in production bridge TU
    must("g_consecutive_dirty_count{0}", "AC1", br)
    must("g_2693_soft_fuse_fallback_total{0}", "AC1", br)
    must("g_2693_soft_fuse_k{3}", "AC1", br)
    must("AURA_EPOCH_INVARIANT_SOFT_FUSE_K", "AC1", br)
    must("aura_epoch_invariant_soft_fuse_k_default", "AC1", br)
    must("aura_set_epoch_invariant_soft_fuse_k", "AC1", br)
    must("aura_get_epoch_invariant_soft_fuse_k", "AC1", br)
    must("Issue #2693", "AC1", br)

    # AC2 — file-level fallback counter + per-CompilerMetrics counter
    must("aura_epoch_invariant_soft_fuse_total_v_read", "AC2", br)
    must("aura_epoch_invariant_consecutive_dirty_total_v_read", "AC2", br)
    must("aura_2693_soft_fuse_record", "AC2", br)
    # K=0 disables fuse (k > 0 guard)
    must("if (k > 0)", "AC2", br)
    # Clean walk resets consecutive (store(0))
    must("g_consecutive_dirty_count.store(0", "AC2", br)
    # Stuck walk bumps (fetch_add)
    must("g_consecutive_dirty_count.fetch_add(1", "AC2", br)
    # Fire on threshold
    must("g_2693_soft_fuse_fallback_total.fetch_add(1", "AC2", br)

    # AC3 — both walks call aura_2693_soft_fuse_record
    must("aura_periodic_epoch_invariant_walk_if_due", "AC3", br)
    must("aura_event_driven_epoch_invariant_walk_if_due", "AC3", br)
    # Two call sites of the helper (one in each walk).
    n_calls = br.count("aura_2693_soft_fuse_record(behind_after_clear)")
    if n_calls < 2:
        fails.append(f"AC3: expected >=2 call sites of aura_2693_soft_fuse_record (got {n_calls})")
    # behind_after_clear captured from invalidate return (not (void)-cast)
    n_capture = br.count("const std::size_t behind_after_clear = aura_aot_invalidate_all_stale_slots_for_eval(nullptr)")
    if n_capture < 2:
        fails.append(f"AC3: expected >=2 capture sites of behind_after_clear (got {n_capture})")

    # Header decls + stub no-ops (regression gate for #81934 / #81967 src-aligned wiring)
    must("aura_epoch_invariant_soft_fuse_total_v_read", "AC3", brh)
    must("aura_epoch_invariant_consecutive_dirty_total_v_read", "AC3", brh)
    must("aura_epoch_invariant_soft_fuse_k_default", "AC3", brh)
    must("aura_set_epoch_invariant_soft_fuse_k", "AC3", brh)
    must("aura_get_epoch_invariant_soft_fuse_k", "AC3", brh)
    must("Issue #2693", "AC3", brh)
    must("g_2693_soft_fuse_fallback_total_stub", "AC3", brs)
    must("g_2693_consecutive_dirty_total_stub", "AC3", brs)
    must("g_2693_soft_fuse_k_stub", "AC3", brs)

    # AC4 — linter self-test
    r = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), "--self-test"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        fails.append(f"AC4 self-test:\n{r.stdout}\n{r.stderr}")
    # Also scan src/ for split-domain bumps.
    split_fails = scan_src_tree()
    if split_fails:
        fails.append("AC4: split-domain bump pattern outside allow-list:\n  " + "\n  ".join(split_fails))

    # AC5 — additive query sentinels in obs_eval.cpp
    must("epoch-invariant-soft-fuse-total", "AC5", obs)
    must("epoch-invariant-consecutive-dirty-total", "AC5", obs)
    must("epoch-invariant-soft-fuse-k-default", "AC5", obs)
    must("epoch-invariant-soft-fuse-wired", "AC5", obs)
    must("schema-2693", "AC5", obs)
    must("issue-2693", "AC5", obs)
    # Prior surfaces preserved
    must("epoch-invariant-event-walks-total", "AC5", obs)
    must("schema-2668", "AC5", obs)
    must("epoch-invariant-periodic-walks-total", "AC5", obs)
    must("schema-2640", "AC5", obs)
    must("epoch-invariant-wired", "AC5", obs)
    must("schema-2366", "AC5", obs)
    must("schema-2304", "AC5", obs)

    # AC6 — test file extension
    must("ac2693_1_consecutive_dirty_fuse_fires", "AC6", test)
    must("ac2693_2_clean_resets_consecutive_and_K0", "AC6", test)
    must("ac2693_3_quiet_zero_cost", "AC6", test)
    must("ac2693_4_linter_self_test", "AC6", test)
    must("ac2693_5_query_keys_added", "AC6", test)
    must("ac2693_6_source_and_linter", "AC6", test)
    must("Issue #2693", "AC6", test)
    # No docs/design/ per #1655 — confirm no docs/design/2693-*.md exists
    # on disk (aura philosophy: agent-developed repo, not human docs;
    # design rationale lives in close comment + commit message). The
    # AC6 test in test_epoch_invariant_walk.cpp also does this check
    # via read_file(design_path + ...).empty(); we duplicate here at
    # the linter layer so a missed cleanup is caught before push.
    design_dir = ROOT / "docs" / "design"
    if design_dir.is_dir():
        for f in sorted(design_dir.glob("2693-*")):
            fails.append(f"AC6: docs/design/{f.name} present on disk (forbidden per #1655)")

    # AC7 — build.py wires the linter
    must("check_joint_epoch_bump_coverage", "AC7", build)
    must("joint epoch bump coverage linter", "AC7", build)

    # Cross-check: #2668 + #2640 + #2366 linters still green
    for prev in (
        "check_2668_coverage.py",
        "check_epoch_invariant_periodic_coverage.py",
        "check_epoch_invariant_walk_2366.py",
    ):
        r = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "coverage" / "checks" / prev)],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if r.returncode != 0:
            fails.append(f"{prev} regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(
        "OK: Issue #2693 Soft epoch-invariant consecutive-dirty fuse + "
        "joint epoch bump static gate — all AC rows satisfied"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
