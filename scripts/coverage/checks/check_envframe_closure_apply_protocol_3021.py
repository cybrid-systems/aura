#!/usr/bin/env python3
"""Issue #3021: unguarded EnvFrame/Closure apply is a forced lifetime protocol.

Contract (one row per AC):
  AC1  Closure apply / EnvFrame use-site: freed or tombstone → reject/skip
       (same skip as scan_skip_freed). Named closure_apply_use_site_ok;
       EnvFrameRef::still_valid rejects INVALID_VERSION.
  AC2  Unguarded bypass inventory: eval_flat TCO + eval_data_as_code are
       not bare apply; aura_closure_call refuses g_closure_freed.
       Production apply goes through apply_closure or the named check.
  AC3  densify ownership scan fail continues to gate would_allow_commit.
  AC4  Tests: steal×compact + EnvFrame truncate suite + freed-closure
       re-apply canary. Extends test_scan_skip_freed_closures +
       test_envframe_truncate_epoch. No test_issue_3021.cpp.
  AC5  Soft extra cost is one lifetime_version load (no Guard, no
       live-closure walk). No second lifetime model. No docs/design/.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _slice_after(hay: str, needle: str, n: int = 4000) -> str:
    i = hay.find(needle)
    if i < 0:
        return ""
    return hay[i : i + n]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ev = _read("src/compiler/evaluator.ixx")
    flat = _read("src/compiler/evaluator_eval_flat.cpp")
    env = _read("src/compiler/evaluator_env.cpp")
    jit = _read("src/compiler/aura_jit_runtime.cpp")
    proof = _read("src/core/envframe_lifetime.ixx")
    scan_t = _read("tests/compiler/test_scan_skip_freed_closures.cpp")
    trunc_t = _read("tests/compiler/test_envframe_truncate_epoch.cpp")
    build = _read("build.py")

    # ── AC1: named protocol ──
    must("Issue #3021", "AC1", ev)
    must("closure_apply_use_site_ok", "AC1 helper", ev)
    must("g_closure_apply_use_site_reject_total", "AC1 counter", ev)
    must("bool Evaluator::closure_apply_use_site_ok", "AC1 def", flat)
    must("lifetime_valid_for_views", "AC1 tombstone", flat)
    must("is_env_frame_invalid(index)", "AC1 EnvFrame INVALID_VERSION", env)
    must("scan_skip_freed", "AC1 same skip", ev)

    apply = _slice_after(flat, "std::optional<EvalValue> Evaluator::apply_closure", 12000)
    must("closure_apply_use_site_ok", "AC1 apply_closure uses protocol", apply)

    # ── AC2: no bare apply ──
    must("no bare TCO apply", "AC2 TCO", flat)
    must("eval_data_as_code: freed/tombstoned closure", "AC2 eval_data_as_code", flat)
    must("never re-apply a slot apply_closure already", "AC2 fallback", flat)
    must("Issue #1361 / #3021", "AC2 aura_closure_call", jit)
    must("g_closure_freed", "AC2 JIT freed table", jit)
    if "AgentRegistry" in flat[flat.find("Issue #3021") : flat.find("Issue #3021") + 2500]:
        fails.append("AC2: must not introduce AgentRegistry")

    # ── AC3: densify proof axis ──
    must("would_allow_commit = !(fail_densify || fail_mismatch)", "AC3 proof", proof)
    must("densify_scan_fail", "AC3 scan fail field", proof)
    must("AC3021: densify scan fail gates would_allow_commit", "AC3 test", trunc_t)

    # ── AC4: tests ──
    must("ac3021_freed_reapply_canary", "AC4 canary", scan_t)
    must("AC3021: freed/tombstone re-apply rejected", "AC4 apply reject", scan_t)
    must("ac3021_envframe_apply_protocol", "AC4 truncate suite", trunc_t)
    must("AC3021: steal×compact FiberSteal Guard", "AC4 steal", trunc_t)
    must("AC3021: steal×compact CompactSweep Guard", "AC4 compact", trunc_t)
    must("check_envframe_closure_apply_protocol_3021", "AC4 build", build)
    must("cmd_envframe_closure_apply_protocol_3021", "AC4 build cmd", build)
    for rel in (
        "tests/compiler/test_issue_3021.cpp",
        "tests/core/test_issue_3021.cpp",
    ):
        if _read(rel):
            fails.append(f"AC4: {rel} exists — forbidden per #81967")

    # ── AC5: Soft / no second model ──
    must("no Guard", "AC5 Soft no Guard", ev)
    must("lifetime_version load", "AC5 Soft cost", ev)
    must("No second lifetime model", "AC5 no second model", ev)
    if _read("docs/design/3021-envframe-closure-apply-protocol.md"):
        fails.append("AC5: docs/design/ exists — forbidden per #1655")

    if fails:
        print(f"Issue #3021 linter FAILED ({len(fails)} rows):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3021 EnvFrame/Closure apply/use-site protocol — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
