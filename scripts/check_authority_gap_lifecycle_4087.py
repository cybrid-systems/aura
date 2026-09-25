#!/usr/bin/env python3
"""Issue #4087: nested-mutate authority gap lifecycle (P0).

Race: nested Guard success (real Guard nesting) stamps the workspace
authority gap (note_nested_authority_gap +
force_query_epoch_stale_from_restamp_budget, production/Full only); the ONLY
clear is the outermost Guard dtor (unified_restamp_after_boundary then
clear_nested_authority_gap). Default agent bodies run on a fiber with an
orch soft checkpoint frame pushed (orch_soft_boundary_enter): pre-#4089 the
first real write's Guard saw prev = size()+1 != 1, was misclassified nested,
and its exit stamped the gap the soft window never cleared — an immediate
query:pattern on an untouched Define printed restamp-lag after a SUCCESSFUL
write, and nested_authority_gap() stayed true after the body returned.

Root fix (already at HEAD via #4089 / 34df1d964): the Guard ctor classifies
the frames below (orch_soft_frame) and flips to the type-authority outermost
when every frame below is soft, so the first real write's dtor owns the
triad + gap clear and the nested-exit stamp fires only for real Guard
nesting. No new query API, no old query key change, Soft/Off never enter the
production/Full arm. This ship adds the issue's verify arm as regression
coverage.

Contract (one row per AC):
  AC1  nested-exit gap stamp arm is gated production/Full; the stamp fires
       before the #3041 QueryEpoch poison (Soft/Off zero extra)
  AC2  the only gap clear is the outermost dtor, after the ctor-captured
       is_outermost_ (#2120); gap-open window telemetry retained
  AC3  soft-only-below flip precedes the is_outermost_ capture, so the
       first real write's Guard is outermost (dtor clears) while real Guard
       nesting (any non-soft frame below) keeps the nested determination;
       the runtime verify arm (query after a successful agent-body write
       returns schema-2, not restamp-lag; gap false after body return)
       lives in tests/compiler/test_hygiene_mutate_closed_loop.cpp
  AC4  build.py wires check_authority_gap_lifecycle_4087 + root allowlist;
       source-cite ACs extend tests/serve/test_orch_soft_boundary_unified.cpp
       (#4087 AC16-AC19); no tests/**/test_issue_4087.cpp; no
       docs/design/4087-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/serve/test_orch_soft_boundary_unified.cpp")
    loop = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # ── AC1: nested-exit stamp arm gated production/Full ─────────────────
    must("workspace_flat_->note_nested_authority_gap();", "AC1 gap stamp", emb)
    must(
        "aura::core::force_query_epoch_stale_from_restamp_budget();",
        "AC1 QueryEpoch poison",
        emb,
    )
    must(
        "if (success && (typed_audit::production_defaults_active() ||",
        "AC1 production/Full gate",
        emb,
    )
    must("Soft / Off: zero extra", "AC1 Soft/Off zero-extra cite", emb)
    gate = emb.find("if (success && (typed_audit::production_defaults_active() ||")
    stamp = emb.find("workspace_flat_->note_nested_authority_gap();")
    poison = emb.find("aura::core::force_query_epoch_stale_from_restamp_budget();")
    if gate == -1 or stamp == -1 or poison == -1 or not (gate < stamp < poison):
        fails.append("AC1: gate must precede the stamp; poison follows the stamp")

    # ── AC2: the only clear is the outermost dtor ────────────────────────
    must("const bool outermost = is_outermost_;", "AC2 ctor-captured outermost (#2120)", emb)
    must("ws->clear_nested_authority_gap();", "AC2 dtor clears the gap", emb)
    must("nested_authority_gap_open_ns", "AC2 gap window telemetry", emb)
    must("Issue #3196: outermost triad published", "AC2 #3196 cite", emb)
    capture = emb.find("const bool outermost = is_outermost_;")
    clear = emb.find("ws->clear_nested_authority_gap();")
    if capture == -1 or clear == -1 or capture > clear:
        fails.append("AC2: clear must run after the outermost capture in the dtor")

    # ── AC3: soft-only-below flip routes the first write outermost ───────
    must("if (soft_only_below)\n            outermost = true;", "AC3 soft-only-below flip", emb)
    must("c4089.orch_soft_frame", "AC3 non-soft frames keep real nesting", emb)
    must("Issue #4089", "AC3 root fix cites #4089", emb)
    flip = emb.find("if (soft_only_below)\n            outermost = true;")
    capture2 = emb.find("is_outermost_ = outermost;")
    if flip == -1 or capture2 == -1 or flip > capture2:
        fails.append("AC3: flip must precede the ctor-captured is_outermost_")
    must("ac4087_1_agent_body_write_query_schema2", "AC3 runtime verify arm", loop)
    must("kQueryResultMatchSchema2Prod", "AC3 runtime arm pins the schema-2 face", loop)
    must("orch_soft_frame = true;", "AC3 runtime arm simulates the soft window", loop)
    must("nested_authority_gap()", "AC3 runtime arm checks the gap face", loop)

    # ── AC4: wiring + test extension + no new files ──────────────────────
    must("check_authority_gap_lifecycle_4087", "AC4 build.py wires the linter", build)
    must("check_authority_gap_lifecycle_4087.py", "AC4 root allowlist carries the linter", allow)
    must("--- #4087 AC16", "AC4 AC16 header present", test)
    must("--- #4087 AC17", "AC4 AC17 header present", test)
    must("--- #4087 AC18", "AC4 AC18 header present", test)
    must("--- #4087 AC19", "AC4 AC19 header present", test)
    must("run_test_orch_soft_boundary_unified", "AC4 ACs in soft-boundary batch", test)
    if _read("tests/compiler/test_issue_4087.cpp"):
        fails.append("AC4: tests/compiler/test_issue_4087.cpp must not exist (#81934)")
    if _read("tests/core/test_issue_4087.cpp"):
        fails.append("AC4: tests/core/test_issue_4087.cpp must not exist (#81934)")
    if (ROOT / "docs" / "design").is_dir() and list((ROOT / "docs" / "design").glob("4087-*")):
        fails.append("AC4: docs/design/4087-* must not exist (#1655)")

    if fails:
        for f in fails:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print("check_authority_gap_lifecycle_4087: OK (4 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
