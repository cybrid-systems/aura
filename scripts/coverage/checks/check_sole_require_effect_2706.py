#!/usr/bin/env python3
"""Issue #2706: sole public side-effect gate = require_effect / require_effect_on_ref.

Contract:
  AC1 Inventory of side-effect prims (mutate / ffi / network / exec / render /
      file-write / hotpath) — each routes through require_effect or
      require_effect_on_ref (source-cite table + no bare Evaluator method).
  AC2 Direct check_and_record_effect( from a primitives TU fails this linter
      (and is private on Evaluator — only security TU defines it).
  AC3 Foreign StableNodeRef under Restricted/Strict → deny before body
      (#2658/#2689 preserved).
  AC4 Same-tenant + grant → allow; Soft / Off path unchanged.
  AC5 Additive query: schema-2706 / issue-2706 / sole-require-effect-gate-armed;
      #2490/#2658/#2689 surfaces preserved.
  AC6 Source-cite + coverage linter; extend test_require_effect_auto_isolation;
      no docs/design/* per #1655.

Scan rules:
  - Forbid bare ``check_and_record_effect(`` in production TUs except:
      * src/compiler/evaluator_security.cpp (definition + require_effect body)
      * comments / string literals (line-level // strip)
  - Allow ``check_and_record_effect_for_test(`` only under tests/
  - Allow free-function ``aura::core::capability::check_and_record_effect``
    (core capability model; not Evaluator API) — matched only when not a
    member call (no preceding '.' or '->').

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Production TUs that must not call Evaluator::check_and_record_effect.
PROD_GLOBS = [
    "src/compiler/evaluator_primitives_*.cpp",
    "src/compiler/ffi_*.cpp",
    "src/compiler/ffi_*.hh",
    "src/compiler/render_*.cpp",
    "src/compiler/render_*.hh",
    "src/compiler/evaluator_fiber_mutation.cpp",
    "src/compiler/evaluator_mutation_boundary.cpp",
    "src/compiler/evaluator_eval_flat.cpp",
    "src/compiler/security_side_effect.hh",
    "src/compiler/security_capabilities.h",
    "src/serve/*.cpp",
    "src/exec/*.cpp",
    "src/orch/*.cpp",
    "src/orch/*.h",
]

# Security implementation is the only production TU that may mention the
# private method (definition + require_effect body + for_test wrapper).
ALLOW_BARE_CALL = {
    "src/compiler/evaluator_security.cpp",
    "src/compiler/evaluator.ixx",  # private decl + for_test decl
}

# Free-function capability API (not Evaluator) — not forbidden.
FREE_FN_NS = re.compile(
    r"(?:aura::core::capability::|using\s+.*check_and_record_effect|"
    r"capability::check_and_record_effect)"
)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _strip_line_comment(line: str) -> str:
    # Drop // comments; keep code (naive — good enough for call-site scan).
    if "//" in line:
        return line[: line.index("//")]
    return line


def _scan_file(rel: str) -> list[str]:
    """Return violation messages for a production file."""
    text = _read(rel)
    if not text:
        return []
    fails: list[str] = []
    if rel in ALLOW_BARE_CALL:
        return fails
    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = _strip_line_comment(raw)
        if "check_and_record_effect" not in line:
            continue
        # Free-function / using import: not Evaluator method.
        if FREE_FN_NS.search(line) or re.search(r"\busing\s+.*\bcheck_and_record_effect\b", line):
            continue
        # Test helper is never allowed in production.
        if "check_and_record_effect_for_test" in line:
            fails.append(f"{rel}:{lineno}: production must not call check_and_record_effect_for_test (tests/ only)")
            continue
        # Member call: .check_and_record_effect( or ->check_and_record_effect(
        if re.search(r"(?:\.|->)\s*check_and_record_effect\s*\(", line):
            fails.append(
                f"{rel}:{lineno}: bare Evaluator::check_and_record_effect — "
                f"use require_effect / require_effect_on_ref (#2706)"
            )
            continue
        # Definition or bare call without ns prefix in evaluator prims TUs.
        if re.search(r"\bcheck_and_record_effect\s*\(", line) and (
            "evaluator_primitives" in rel or "evaluator_" in rel
        ):
            fails.append(
                f"{rel}:{lineno}: bare check_and_record_effect( in evaluator "
                f"TU — use require_effect / require_effect_on_ref (#2706)"
            )
    return fails


def _collect_prod_files() -> list[str]:
    out: list[str] = []
    for g in PROD_GLOBS:
        for p in ROOT.glob(g):
            rel = str(p.relative_to(ROOT)).replace("\\", "/")
            out.append(rel)
    # Always include the known primitives set even if glob misses.
    for rel in (
        "src/compiler/evaluator_primitives_mutate.cpp",
        "src/compiler/evaluator_primitives_io.cpp",
        "src/compiler/evaluator_primitives_security.cpp",
        "src/compiler/evaluator_primitives_file.cpp",
        "src/compiler/evaluator_primitives_compile.cpp",
        "src/compiler/evaluator_primitives_runtime.cpp",
        "src/compiler/evaluator_primitives_messaging.cpp",
        "src/compiler/evaluator_primitives_agent.cpp",
        "src/compiler/security_side_effect.hh",
    ):
        if rel not in out and (ROOT / rel).is_file():
            out.append(rel)
    return sorted(set(out))


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ixx = _read("src/compiler/evaluator.ixx")
    sec = _read("src/compiler/evaluator_security.cpp")
    side = _read("src/compiler/security_side_effect.hh")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    test = _read("tests/compiler/test_require_effect_auto_isolation.cpp")
    build = _read("build.py")
    io_cpp = _read("src/compiler/evaluator_primitives_io.cpp")
    sec_prim = _read("src/compiler/evaluator_primitives_security.cpp")
    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    file_cpp = _read("src/compiler/evaluator_primitives_file.cpp")

    # AC1 — inventory routes through require_effect.
    must("require_effect", "AC1", mut)
    must("require_effect", "AC1", file_cpp)
    must("require_effect", "AC1", io_cpp)
    must("require_effect", "AC1", sec_prim)  # security:check-effect
    must("require_effect", "AC1", sec)
    must("require_effect_on_ref", "AC1", ixx)
    # Source-cite table in test.
    must("#2706", "AC1", test)

    # AC2 — private decl + for_test public + no bare prim calls.
    must("check_and_record_effect_for_test", "AC2", ixx)
    must("check_and_record_effect", "AC2", ixx)  # private decl still present
    must("bool Evaluator::check_and_record_effect", "AC2", sec)
    must("bool Evaluator::check_and_record_effect_for_test", "AC2", sec)
    # require_effect body still calls private method.
    m = re.search(
        r"bool\s+Evaluator::require_effect\s*\([^)]*\)\s*(?:noexcept)?\s*\{(.+?)\n\}",
        sec,
        re.MULTILINE | re.DOTALL,
    )
    if not m:
        # brace-count fallback
        sig = "bool Evaluator::require_effect"
        i = sec.find(sig)
        body = ""
        if i >= 0:
            brace = sec.find("{", i)
            depth = 0
            for j in range(brace, len(sec)):
                if sec[j] == "{":
                    depth += 1
                elif sec[j] == "}":
                    depth -= 1
                    if depth == 0:
                        body = sec[brace + 1 : j]
                        break
        if not body or "check_and_record_effect" not in body:
            fails.append("AC2: require_effect must call private check_and_record_effect")
    elif "check_and_record_effect" not in m.group(1):
        fails.append("AC2: require_effect body must call check_and_record_effect")

    for rel in _collect_prod_files():
        fails.extend(_scan_file(rel))

    # Explicit residual production member-call scan.
    for rel in (
        "src/compiler/evaluator_primitives_io.cpp",
        "src/compiler/evaluator_primitives_security.cpp",
    ):
        t = _read(rel)
        cleaned = t.replace("check_and_record_effect_for_test", "FORTEST")
        if re.search(r"(?:\.|->)\s*check_and_record_effect\s*\(", cleaned):
            fails.append(f"AC2: residual member check_and_record_effect in {rel}")

    # AC3 / AC4 — lineage preserved in security TU + #2689/#2490 linters still wired.
    must("require_effect_on_ref", "AC3", sec)
    must("#2658", "AC3", sec)
    must("#2689", "AC3", sec)
    must("check_workspace_isolation", "AC3", sec)
    must("#2490", "AC4", sec)

    # AC5 — additive query; preserve prior schemas via citations in q or tests.
    must("schema-2706", "AC5", q)
    must("issue-2706", "AC5", q)
    must("sole-require-effect-gate-armed", "AC5", q)
    # Prior #2490/#2658/#2689 lineage preserved (source-cite, not new schema).
    must("#2490", "AC5", sec + test)
    must("#2658", "AC5", sec + test)
    must("#2689", "AC5", sec + test)

    # AC6 — source-cite + no design docs + gate wire + test extend.
    must("#2706", "AC6", ixx)
    must("#2706", "AC6", sec)
    must("#2706", "AC6", side)
    must("#2706", "AC6", q)
    must("#2706", "AC6", test)
    must("check_sole_require_effect_2706", "AC6", build)
    for rel in (
        "docs/design/sole_require_effect_2706.md",
        "docs/sole_require_effect_2706.md",
        "design/2706.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2706 sole public require_effect side-effect gate — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
