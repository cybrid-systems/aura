#!/usr/bin/env python3
"""Issue #2814 M7: capture_audit_event_forced linked to invariant enforcement.

Contract (one row per AC):
  AC1 capture_audit_event_forced gap detection + note_ran/skipped APIs
  AC2 record_invariant_audit_result notes ran; Guard notes intentional skip
  AC3 query schema-2814 + test suite
  AC4 this linter wired; no docs/design/2814-*; no test_issue_2814.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    h = _read("src/compiler/typed_mutation_audit.h")
    bound = _read("src/compiler/evaluator_mutation_boundary.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_capture_audit_event_forced_enforcement_link.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    def_pos = h.find("inline void capture_audit_event_forced")
    win = h[def_pos : def_pos + 3500] if def_pos >= 0 else ""

    # AC1
    must("Issue #2814", "AC1", win)
    must("audit_enforcement_gap_total", "AC1", win)
    must("enforcement_linked_for", "AC1", win)
    must("note_invariant_enforcement_ran", "AC1", h)
    must("note_invariant_enforcement_skipped", "AC1", h)
    must("audit_enforcement_link_wired", "AC1", h)
    must("audit_enforcement_ran_total", "AC1", h)
    must("audit_enforcement_skipped_intentional_total", "AC1", h)

    # AC2
    rec = h.find("inline void record_invariant_audit_result")
    rwin = h[rec : rec + 600] if rec >= 0 else ""
    must("note_invariant_enforcement_ran", "AC2", rwin)
    must("note_invariant_enforcement_skipped", "AC2", bound)
    must("Issue #2814", "AC2", bound)
    # At least two skip sites (render-fast + Sampled quiet).
    if bound.count("note_invariant_enforcement_skipped") < 2:
        fails.append("AC2: expected ≥2 note_invariant_enforcement_skipped in Guard")

    # AC3
    must("schema-2814", "AC3", obs)
    must("audit-enforcement-gap-total", "AC3", obs)
    must("audit-enforcement-link-wired", "AC3", obs)
    must("ac2814", "AC3", test)
    must("2814", "AC3", test)
    must("capture_audit_event_forced", "AC3", test)
    must("audit_enforcement_gap_total", "AC3", test)
    must("note_invariant_enforcement_ran", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_capture_audit_event_forced_enforcement_link.cpp").is_file():
        fails.append("AC3: missing test_capture_audit_event_forced_enforcement_link.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2814.cpp").is_file():
        fails.append("AC3: test_issue_2814.cpp present (forbidden per #81967)")
    must("test_capture_audit_event_forced_enforcement_link", "AC3", cmake)

    # AC4
    must("check_capture_audit_event_forced_enforcement_link_2814", "AC4", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2814-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2814 audit enforcement link — gap metric + Guard skip notes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
