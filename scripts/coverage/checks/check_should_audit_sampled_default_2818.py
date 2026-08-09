#!/usr/bin/env python3
"""Issue #2818: should_audit Full cold-start default; Sampled opt-in only.

Contract (one row per AC):
  AC1 Full static default; maybe_warn; audit_strategy_default_warnings_total
  AC2 apply_dev sets dev_audit_opt_in; Sampled/4 under-sample path
  AC3 test suite present (prefer-existing / no test_issue_2818.cpp)
  AC4 this linter wired; schema-2818 query; no docs/design/2818-*

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
    obs = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_should_audit_sampled_default.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # AC1
    must("Issue #2818", "AC1", h)
    must("AuditStrategy::Full", "AC1", h)
    must("sample_ratio{1}", "AC1", h)
    must("audit_strategy_default_warnings_total", "AC1", h)
    must("dev_audit_opt_in", "AC1", h)
    must("maybe_warn_sampled_without_opt_in", "AC1", h)
    must("should_audit", "AC1", h)

    # AC2
    must("apply_dev_audit_defaults", "AC2", h)
    must("dev_audit_opt_in.store(1", "AC2", h)
    must("set_strategy(AuditStrategy::Sampled)", "AC2", h)
    must("set_sample_ratio(4)", "AC2", h)

    # AC3
    must("ac2818", "AC3", test)
    must("2818", "AC3", test)
    must("audit_strategy_default_warnings_total", "AC3", test)
    must("should_audit", "AC3", test)
    must("apply_dev_audit_defaults", "AC3", test)
    if not (ROOT / "tests" / "compiler" / "test_should_audit_sampled_default.cpp").is_file():
        fails.append("AC3: missing test_should_audit_sampled_default.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_2818.cpp").is_file():
        fails.append("AC3: test_issue_2818.cpp present (forbidden per #81967)")
    if (ROOT / "tests" / "audit" / "test_should_audit_sampled_default.cpp").is_file():
        # Prefer tests/compiler/ per project layout; audit/ path from issue is
        # optional — fail only if both missing (already checked above).
        pass
    must("test_should_audit_sampled_default", "AC3", cmake)

    # AC4
    must("check_should_audit_sampled_default_2818", "AC4", build)
    must("schema-2818", "AC4", obs)
    must("audit-strategy-default-warnings-total", "AC4", obs)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2818-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2818 should_audit Full default — Sampled opt-in only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
