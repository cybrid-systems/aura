#!/usr/bin/env python3
"""Issue #3436: grant writers share one lifetime policy + caller principal.

Contract (one row per AC; windowed substring checks that survive
clang-format rewrapping — see source-cite-lint-windowing skill):
  AC1 source: Evaluator::grant_capability (single-arg string path) force-
     promotes high-risk bits (#2882 mask) to single_use under production
     (sandbox_mode_ != 0 || effect_sandbox_mode() != 0) and bumps the
     existing capability_high_risk_forced_single_use_total counter.
  AC2 source: the string path delegates with session_bound=false — the
     documented string path never mints session residual rows.
  AC3 source: every grant_effect_* grant()/grant_locked() site passes
     capability_tenant_id_ as caller_principal (>= 7 sites total) so the
     #3409 SSOT fence evaluates the granting Evaluator's principal, not
     the default_tenant fallback.
  AC4 source: grant_effect_durable + _sticky scope the registry mutex
     (mirror-outside-lock markers x2) and all four string mirrors use the
     explicit-lifetime 4-arg grant_capability form (capability keeps the
     wrapper single_use, durable keeps #3177 session_bound, sticky keeps
     false/false, session keeps sb=true).
  AC5 source: the force gate is the evaluator-level OR (Soft/Off zero cost).
  AC6 fixture: tests/core/test_capability_single_use_consume.cpp carries the
     #3436 AC block; evaluator.ixx declares the mirror overload; no
     test_issue_3436.cpp; no docs/design/3436-*; linter wired into build.py.

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

    def at_least(n: int, needle: str, hay: str, label: str) -> None:
        if hay.count(needle) < n:
            fails.append(f"{label}: expected >= {n} of {needle!r}, got {hay.count(needle)}")

    sec = _read("src/compiler/evaluator_security.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    fixture = _read("tests/core/test_capability_single_use_consume.cpp")
    build = _read("build.py")

    def span(start_marker: str, end_marker: str, label: str) -> str:
        i = sec.find(start_marker)
        j = sec.find(end_marker, i + 1) if i >= 0 else -1
        if i < 0 or j < 0:
            fails.append(f"{label}: span markers not found")
            return ""
        return sec[i:j]

    # ---- AC1 + AC2 + AC5: single-arg string path span ----
    s1 = span(
        "void Evaluator::grant_capability(std::string cap) {",
        "void Evaluator::grant_capability(std::string cap, bool single_use",
        "AC1-span",
    )
    if s1:
        must("Issue #3436", "AC1", s1)
        must("kHighRiskMask", "AC1", s1)
        must("kEffectMutate | kEffectMacroSelfEvo | kEffectTenantAdmin | kEffectSyscall", "AC1", s1)
        must("capability_high_risk_forced_single_use_total", "AC1", s1)
        must("production_defaults", "AC1", s1)
        must("sandbox_mode_ != 0 || effect_sandbox_mode() != 0", "AC1", s1)
        # AC5: force fires only inside the production_defaults && high-risk branch.
        must("production_defaults &&", "AC5", s1)
        must("kHighRiskMask) != 0", "AC5", s1)
        # AC2: string path delegates with session_bound=false (no session rows).
        must("grant_capability(std::move(cap), single_use, /*session_bound=*/false,", "AC2", s1)

    CALLER = "static_cast<std::uint64_t>(capability_tenant_id_)"

    # ---- AC3 + AC4: per-wrapper spans ----
    cap_span = span("void Evaluator::grant_effect_capability(", "void Evaluator::grant_effect_durable(", "AC3-span-cap")
    if cap_span:
        at_least(3, CALLER, cap_span, "AC3-cap")
        must("grant_locked(tenant_id, name, static_cast<Effect>(effect_bits)", "AC3-cap-locked", cap_span)
        must("reg.grant(tenant_id, name, static_cast<Effect>(effect_bits)", "AC3-cap-else", cap_span)
        must("grant_capability(std::string(name), single_use, /*session_bound=*/false,", "AC4-cap-mirror", cap_span)

    dur_span = span(
        "void Evaluator::grant_effect_durable(", "void Evaluator::grant_effect_durable_sticky(", "AC3-span-dur"
    )
    if dur_span:
        at_least(1, CALLER, dur_span, "AC3-durable")
        must("force_session", "AC4-durable", dur_span)
        must("grant_capability(std::string(name), /*single_use=*/false, force_session,", "AC4-durable-mirror", dur_span)
        must("Issue #3436: registry lock released BEFORE the string mirror.", "AC4-durable-lockscope", dur_span)

    sticky_span = span(
        "void Evaluator::grant_effect_durable_sticky(", "void Evaluator::grant_effect_session(", "AC3-span-sticky"
    )
    if sticky_span:
        at_least(1, CALLER, sticky_span, "AC3-sticky")
        must(
            "grant_capability(std::string(name), /*single_use=*/false, /*session_bound=*/false,",
            "AC4-sticky-mirror",
            sticky_span,
        )
        must("Issue #3436: registry lock released BEFORE the string mirror.", "AC4-sticky-lockscope", sticky_span)

    sess_span = span(
        "void Evaluator::grant_effect_session(", "void Evaluator::revoke_effect_capability(", "AC3-span-sess"
    )
    if sess_span:
        at_least(2, CALLER, sess_span, "AC3-session")
        must("grant_locked(tenant_id, name, static_cast<Effect>(effect_bits)", "AC3-session-locked", sess_span)
        must("reg_session.grant(tenant_id, name, static_cast<Effect>(effect_bits)", "AC3-session-else", sess_span)
        must("grant_capability(std::string(name), single_use, /*session_bound=*/true,", "AC4-session-mirror", sess_span)

    at_least(7, CALLER, sec, "AC3-total")

    # ---- AC6: fixture + declarations + wiring ----
    must("ac3436_1_string_path_forced_single_use", "AC6", fixture)
    must("Issue #3436", "AC6", fixture)
    must("void grant_capability(std::string cap, bool single_use, bool session_bound,", "AC6-ixx", ixx)
    must("check_grant_lifetime_alignment_3436.py", "AC6-build", build)
    issue_test = ROOT / "tests" / "core" / "test_issue_3436.cpp"
    if issue_test.exists():
        fails.append("AC6: tests/core/test_issue_3436.cpp must not exist")
    design_dir = ROOT / "docs" / "design"
    if design_dir.is_dir():
        bad = [p.name for p in design_dir.iterdir() if p.name.startswith("3436")]
        if bad:
            fails.append(f"AC6: docs/design/ has {bad}")

    if fails:
        print(f"check_grant_lifetime_alignment_3436: {len(fails)} row(s) failed:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_grant_lifetime_alignment_3436: all contract rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
