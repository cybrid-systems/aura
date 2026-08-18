#!/usr/bin/env python3
"""Issue #2882: production default single-use for high-risk grants.

Contract (one row per AC):
  AC1  Production defaults (Restricted/Strict) force single_use=true on
       grant_effect_capability when any high-risk effect bit
       (Mutate | MacroSelfEvo | TenantAdmin | Syscall) is requested.
  AC2  Explicit grant_effect_durable admin path bypasses the force and
       bumps capability_durable_high_risk_grant_total.
  AC3  Off / Soft path: production defaults off → no force applied;
       existing single_use=false caller intent honored.
  AC4  Existing #2586 ACs remain green: single_use flag still forwarded
       to registry::grant; consume block still emits
       'single-use-consumed' SE reason.
  AC5  Agent-visible metric + SE reason stable; query surface additive
       (schema-2882 + capability-high-risk-forced-single-use-total +
       capability-durable-high-risk-grant-total).
  AC6  Source-cite + tests; no docs/design/; no new test_issue_2882.cpp.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Files in scope for #2882 (production-default single-use override).
SCOPE_FILES = [
    "src/core/capability_model.hh",
    "src/compiler/evaluator_security.cpp",
    "src/compiler/evaluator.ixx",
    "src/compiler/evaluator_primitives_security.cpp",
    "src/compiler/security_capabilities.h",
    "tests/core/test_capability_single_use_consume.cpp",
    "scripts/coverage/checks/check_production_default_single_use_2882.py",
]

# High-risk effect bit mask the production default must override.
# Must match the constexpr kHighRiskMask in evaluator_security.cpp +
# evaluator_primitives_security.cpp + capability_model.hh consumers.
HIGH_RISK_MASK_VALUE = (
    (1 << 3)  # kEffectMutate
    | (1 << 7)  # kEffectMacroSelfEvo
    | (1 << 8)  # kEffectTenantAdmin
    | (1 << 9)  # kEffectSyscall
)


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

    cap_model = _read("src/core/capability_model.hh")
    sec = _read("src/compiler/evaluator_security.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    posture = _read("src/compiler/evaluator_primitives_security.cpp")
    sec_caps = _read("src/compiler/security_capabilities.h")
    test_su = _read("tests/core/test_capability_single_use_consume.cpp")
    build = _read("build.py")

    # ── AC1: production default forces single_use for high-risk ─────
    must("Issue #2882", "AC1", sec)
    must("production default single-use", "AC1", sec)  # the comment block
    must("kHighRiskMask", "AC1", sec)
    # Force logic must precede the registry::grant() single_use forwarding.
    cap_p = sec.find("kHighRiskMask")
    # Issue #3126: after the locked-variant refactor (TOCTOU admin fence
    # fix), the foreign-tenant path calls `reg.grant_locked(...)` under
    # the registry mtx; the same-tenant path calls `reg.grant(...)` (which
    # forwards single_use to grant_locked internally). Both forms forward
    # single_use to the registry's mutation logic — the #2882 contract is
    # preserved, the call site style changed.
    grant_idx = sec.find("g_capability_registry().grant(tenant_id, name")
    if grant_idx < 0:
        grant_idx = sec.find(".grant_locked(tenant_id, name")
    if cap_p < 0:
        fails.append("AC1: kHighRiskMask not defined in evaluator_security.cpp")
    if grant_idx < 0:
        fails.append("AC1: registry::grant(...) single_use forwarding missing")
    if cap_p >= 0 and grant_idx >= 0 and cap_p > grant_idx:
        fails.append("AC1: kHighRiskMask defined AFTER registry::grant(...) — force logic must precede grant call")
    # high-risk bit constants must exist (per security_capabilities.h).
    for bit in ("kEffectMutate", "kEffectMacroSelfEvo", "kEffectTenantAdmin", "kEffectSyscall"):
        must(bit, "AC1", sec_caps)
    # Counter must be bumped when force applied.
    must("capability_high_risk_forced_single_use_total", "AC1", cap_model)
    must("capability_high_risk_forced_single_use_total", "AC1", sec)

    # ── AC2: explicit durable admin path still works ─────────────────
    must("grant_effect_durable", "AC2", sec)
    must("grant_effect_durable", "AC2", ixx)
    # durable counter must be bumped on high-risk durable override.
    must("capability_durable_high_risk_grant_total", "AC2", cap_model)
    must("capability_durable_high_risk_grant_total", "AC2", sec)
    # grant_effect_durable must NOT touch capability_high_risk_forced_single_use_total
    # (durable override is the audited bypass, not the forced-on path).
    durable_block = (
        sec[sec.find("Evaluator::grant_effect_durable") : sec.find("Evaluator::grant_effect_durable") + 1500]
        if "Evaluator::grant_effect_durable" in sec
        else ""
    )
    if "capability_high_risk_forced_single_use_total" in durable_block:
        fails.append(
            "AC2: grant_effect_durable bumps capability_high_risk_forced_single_use_total "
            "— durable override should NOT increment the forced-on counter"
        )

    # ── AC3: Off / Soft unchanged (no force applied) ─────────────────
    must("production_defaults", "AC3", sec)  # local variable name
    must("force_bind", "AC3", sec)  # existing pattern reused
    # force_bind false path → no force (the conditional `if (production_defaults && is_high_risk && !single_use)`)
    must("production_defaults && is_high_risk", "AC3", sec)

    # ── AC4: existing #2586 paths remain green ──────────────────────
    must("single_use", "AC4", sec)
    must("Issue #2586", "AC4", cap_model)
    must("single-use-consumed", "AC4", cap_model)
    must("grant_once", "AC4", cap_model)
    must("single_use = single_use", "AC4", cap_model)  # re-grant semantics
    # check_and_record_effect consume block must still be present.
    must("capability_single_use_consumed_total", "AC4", cap_model)

    # ── AC5: metric + SE reason stable; query surface additive ──────
    must("schema-2882", "AC5", posture)
    must("issue-2882", "AC5", posture)
    must("production-default-single-use-wired", "AC5", posture)
    must("high-risk-default-single-use-mask", "AC5", posture)
    must("capability-high-risk-forced-single-use-total", "AC5", posture)
    must("capability-durable-high-risk-grant-total", "AC5", posture)
    # Constants must match inventory: high-risk-default-single-use-mask == HIGH_RISK_MASK_VALUE.
    # The mask value is sourced from a `kHighRiskMask` constexpr in
    # evaluator_security.cpp + evaluator_primitives_security.cpp — verify
    # both files name the 4 high-risk bits so the OR is correct.
    for bit_name in ("kEffectMutate", "kEffectMacroSelfEvo", "kEffectTenantAdmin", "kEffectSyscall"):
        must(bit_name, "AC5", sec)  # kHighRiskMask defined in sec (force site)
        must(bit_name, "AC5", posture)  # also referenced in posture (query site)
    must("kHighRiskMask", "AC5", sec)
    must("kHighRiskMask", "AC5", posture)
    # Verify the high-risk mask in evaluator_security.cpp matches the
    # canonical HIGH_RISK_MASK_VALUE used by the linter.
    sec_mask_match = re.search(
        r"constexpr\s+std::uint16_t\s+kHighRiskMask\s*=\s*static_cast<std::uint16_t>\s*\(\s*"
        r"(kEffect\w+)\s*\|\s*(kEffect\w+)\s*\|\s*(kEffect\w+)\s*\|\s*(kEffect\w+)\s*\)",
        sec,
    )
    if sec_mask_match:
        bits = set(sec_mask_match.groups())
        expected_bits = {"kEffectMutate", "kEffectMacroSelfEvo", "kEffectTenantAdmin", "kEffectSyscall"}
        if bits != expected_bits:
            fails.append(
                f"AC5: kHighRiskMask in evaluator_security.cpp = {sorted(bits)} != expected {sorted(expected_bits)}"
            )
    else:
        fails.append(
            "AC5: kHighRiskMask constexpr in evaluator_security.cpp does not match "
            "expected 4-bit OR (kEffectMutate|kEffectMacroSelfEvo|kEffectTenantAdmin|kEffectSyscall)"
        )
    # Snapshot struct must carry both new fields.
    must("capability_high_risk_forced_single_use", "AC5", cap_model)
    must("capability_durable_high_risk_grant", "AC5", cap_model)

    # ── AC6: source-cite + tests; no docs/design/; no new test_issue_2882.cpp ─
    must("Issue #2882", "AC6", cap_model)
    must("#2882", "AC6", ixx)  # lineage cite
    must("2882", "AC6", test_su)  # tests cite #2882
    must("build", "AC6", build)
    # build.py wires this linter.
    if "check_production_default_single_use_2882" not in build:
        fails.append("AC6: build.py does not wire #2882 linter")
    # No new test_issue_2882.cpp (per #81967).
    if (ROOT / "tests" / "core" / "test_issue_2882.cpp").is_file():
        fails.append("AC6: test_issue_2882.cpp present (forbidden per #81967)")
    if (ROOT / "tests" / "compiler" / "test_issue_2882.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_2882.cpp present (forbidden per #81967)")
    # No docs/design/2882-* (per #1655).
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2882-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # ── Cross-check #2586 linter still green ─────────────────────────
    r = subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts" / "coverage" / "checks" / "check_capability_single_use_2586.py"),
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    # Older #2586 may not have a dedicated linter — accept either presence
    # (exit 0) or absence (FileNotFoundError -> capture in stderr).
    if r.returncode != 0 and "No such" not in r.stderr and "FileNotFoundError" not in r.stderr:
        # Non-fatal: just record a warning if #2586 linter regressed.
        fails.append(f"check_capability_single_use_2586 regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2882 production default single-use for high-risk grants")
    return 0


if __name__ == "__main__":
    sys.exit(main())
