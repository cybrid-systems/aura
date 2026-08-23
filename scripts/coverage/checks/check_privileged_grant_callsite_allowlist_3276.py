#!/usr/bin/env python3
"""Issue #3276: freeze the privileged-write call-site allowlist.

Residual of #2968/#3086/#3145/#3029/#2969/#3141: runtime fences on
Evaluator paths are solid, but any NEW translation unit can call the
process-global capability registry / workspace-isolation grant family and
bypass Evaluator principal / audit / TenantAdmin wrappers — bypass by
addition, not by broken existing fence.

This check scans src/ (production TUs) for direct privileged-write call
patterns and fails the gate when any hit falls OUTSIDE the frozen
allowlist (scripts/coverage/allowlists/privileged_grant_calls.json).

Allowlist (sole permitted inventory):
  - src/compiler/evaluator_security.cpp     .grant( .grant_locked(
                                             .grant_session( .grant_cross_tenant(
  - src/compiler/security_defaults.hh       .grant(  (bootstrap render, tenant=0)
  - src/compiler/evaluator_primitives_security.cpp  .grant_macro_self_evo(
                                             .revoke_macro_self_evo(

Out of scope (NOT grant writes):
  - read-only getters grant_epoch_retain_window() / grant_min_valid_epoch()
    (evaluator_primitives_obs_jit.cpp) — never matched by the scanned
    patterns below (they are `.grant_epoch_retain_window(` etc.)
  - method definitions inside capability_model.hh / workspace_isolation.hh
    (declarations, not call sites — no `.grant(` call form)
  - tests/ (Soft / explicit test helpers are exempt)

Contract (one row per AC):
  AC1  allowlist file exists + lists the 3 production TUs with the
       grant / grant_locked / grant_session / grant_once /
       grant_macro_self_evo / revoke_macro_self_evo / grant_cross_tenant
       patterns (read-only getters excluded)
  AC2  every scanned pattern hit in src/ is inside an allowlisted file;
       any hit outside → gate fail (new TU direct grant blocked)
  AC3  allowlisted files still contain their permitted call sites
       (allowlist does not silently rot)
  AC4  no new query:* / no second capability model / no docs/design/
       (#1655); tests extend existing src-aligned suite (#81967)
  AC5  build.py wires this linter

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ALLOWLIST = ROOT / "scripts" / "coverage" / "allowlists" / "privileged_grant_calls.json"

# Grant-family call forms to scan for. Read-only getters are NOT matched
# (they are .grant_epoch_retain_window( / .grant_min_valid_epoch( — the
# pattern requires `grant(` / `grant_locked(` etc. immediately after the
# dot, so `grant_epoch...` never matches).
SCANNED_PATTERNS = [
    ".grant(",
    ".grant_locked(",
    ".grant_session(",
    ".grant_once(",
    ".grant_macro_self_evo(",
    ".revoke_macro_self_evo(",
    ".grant_cross_tenant(",
]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _strip_comments(src: str) -> str:
    """Strip // line comments and /* */ block comments so doc mentions of
    grant_cross_tenant (e.g. workspace_isolation.hh) are not false hits."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    src = re.sub(r"//[^\n]*", "", src)
    return src


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    # ── AC1: allowlist file exists + inventory ──
    if not ALLOWLIST.is_file():
        fails.append("AC1: allowlist file missing")
        return 1
    al = json.loads(ALLOWLIST.read_text(encoding="utf-8"))
    files = al.get("files", {})
    must("src/compiler/evaluator_security.cpp", "AC1", str(files))
    must("src/compiler/security_defaults.hh", "AC1", str(files))
    must("src/compiler/evaluator_primitives_security.cpp", "AC1", str(files))
    sec = files.get("src/compiler/evaluator_security.cpp", {}).get("allowed", [])
    for pat in [".grant(", ".grant_locked(", ".grant_session(", ".grant_cross_tenant("]:
        if pat not in sec:
            fails.append(f"AC1: evaluator_security.cpp allowlist missing {pat!r}")
    sdef = files.get("src/compiler/security_defaults.hh", {}).get("allowed", [])
    if ".grant(" not in sdef:
        fails.append("AC1: security_defaults.hh allowlist missing .grant(")
    prim = files.get("src/compiler/evaluator_primitives_security.cpp", {}).get("allowed", [])
    if ".grant_macro_self_evo(" not in prim:
        fails.append("AC1: evaluator_primitives_security.cpp allowlist missing .grant_macro_self_evo(")
    if ".grant_epoch_retain_window(" in str(files) or ".grant_min_valid_epoch(" in str(files):
        fails.append("AC1: read-only getters must NOT be in the allowlist (out of scope)")

    # ── AC2: scan src/ — every pattern hit must be inside an allowlisted file ──
    allowed_files = set(files.keys())
    src_root = ROOT / "src"
    hits_outside: list[str] = []
    for path in sorted(src_root.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix not in (".cpp", ".hh", ".h", ".ixx"):
            continue
        rel = path.relative_to(ROOT).as_posix()
        text = _strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for pat in SCANNED_PATTERNS:
            if pat in text:
                if rel not in allowed_files:
                    hits_outside.append(f"{rel}: contains {pat!r}")
                else:
                    allowed_pats = files[rel].get("allowed", [])
                    if pat not in allowed_pats:
                        hits_outside.append(f"{rel}: {pat!r} not in its allowlist row")
    for h in hits_outside:
        fails.append(f"AC2: {h}")

    # ── AC3: allowlisted files still contain their permitted call sites ──
    # AC3 requires at least one permitted pattern per allowlisted file (the
    # file is still the authority for its granted surface). Permitted-but-
    # currently-unused forms (e.g. .grant_session( — Evaluator calls it via
    # reg_session.grant_locked/grant; .revoke_macro_self_evo( — prim path
    # only grants today) do NOT need to be present today; they are frozen
    # as permitted for future legitimate use inside the same authority.
    for rel, row in files.items():
        src_text = _strip_comments(_read(rel))
        allowed_pats = row.get("allowed", [])
        if not any(pat in src_text for pat in allowed_pats):
            fails.append(f"AC3: {rel} contains none of its permitted grant patterns")

    # ── AC4 / AC5: no invent + build.py wiring ──
    build = _read("build.py")
    if "check_privileged_grant_callsite_allowlist_3276" not in build:
        fails.append("AC5: build.py does not wire check_privileged_grant_callsite_allowlist_3276")
    if _read("tests/core/test_issue_3276.cpp") or _read("tests/issues/test_issue_3276.cpp"):
        fails.append("AC4: test_issue_3276.cpp present (forbidden #81967)")
    if _read("docs/design/3276-privileged-grant-allowlist.md"):
        fails.append("AC4: docs/design/ exists — forbidden per #1655")

    if fails:
        print("FAIL #3276 privileged_grant_callsite_allowlist:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3276 privileged_grant_callsite_allowlist: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
