#!/usr/bin/env python3
r"""Issue #2709: GeneralObjectPin mandatory coverage beyond inventory-of-7.

Closes the partial-adoption gap: the original #2496 inventory was a hardcoded
list (kGeneralObjectPinAdoptSiteCount = 7) — new create paths could allocate
densify-tracked intermediates without `wire_general_object_create_pair` or
`GENERAL_OBJECT_PIN_EXEMPT(reason)`, opening a Moving densify UAF / fail-closed
noise window. #2709 replaces the static list with a dynamic count
(general_object_pin_auto_wire_total + general_object_pin_exempt_total) and
adds a coverage linter that fails when a new create site under
`src/compiler/evaluator_primitives_*.cpp` / `evaluator_eval_flat.cpp`
allocates without wire or EXEMPT.

Contract rows (AC1–AC6 from the test file):

  AC1: production paths either wire or EXEMPT (source-cite helper + counters)
  AC2: linter fails on new create site without wire/EXEMPT
  AC3: Soft / sandbox=off / Moving off → zero extra cost
  AC4: required-mode fail-closed (already covered by #2665 — regression only)
  AC5: additive query keys (auto_wire_total, exempt_total, adopt_site_count)
  AC6: source-cite + linter + schema-2709 + no docs/design/

Exit 0 = all contract rows satisfied.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _self_test() -> int:
    """Run the linter on a known-good tree and verify exit 0.

    The default tree IS the known-good tree for #2709 (this linter is the
    source of truth on the contract rows — if the tree is healthy, the
    linter should report OK). If the tree regresses, the linter will report
    fails and --self-test exits 1.
    """
    r = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "check_general_object_pin_auto_wire_2709.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        print(f"--self-test FAILED:\n{r.stdout}\n{r.stderr}", file=sys.stderr)
        return 1
    print(f"--self-test OK: {r.stdout.strip()}")
    return 0


# AC2 allocate-pattern detector. Scans evaluator_primitives_*.cpp +
# evaluator_eval_flat.cpp for functions whose body contains allocate-like
# patterns but lacks wire_general_object_create_pair or GENERAL_OBJECT_PIN_EXEMPT.
# This is a heuristic — it covers the dominant cases (allocate_raw,
# pool.acquire, push_node / create_node) without doing full AST analysis.
_ALLOCATE_PATTERNS = re.compile(
    r"\b(allocate_raw|allocate\b|alloc\b|\.acquire\(|push_node|create_node|"
    r"pool\.alloc|StringPool::acquire|FlatAST::push)\b"
)
_WIRE_PATTERNS = re.compile(
    r"\b(wire_general_object_create_pair_or_required_fail|"
    r"wire_general_object_create_pair_or_exempt|"
    r"wire_general_object_create_pair|"
    r"note_general_object_pin_mutate_wire)\b"
)
_EXEMPT_PATTERNS = re.compile(r"\bGENERAL_OBJECT_PIN_EXEMPT\s*\(")


def _scan_create_sites_for_missing_wire_or_exempt(files: list[str]) -> list[str]:
    r"""Return list of (file:line) where an allocate-like pattern appears
    without wire or EXEMPT in the enclosing function body.

    Function body detection: heuristic brace-counting from the first '{'
    after a function signature line (line matching `^(static |inline |void
    |bool |.*::.*)\(.*\)\s*\{?`). Sufficient for the dominant aura
    patterns; deep AST analysis is out of scope.
    """
    fails: list[str] = []
    for rel in files:
        body = _read(rel)
        if not body:
            continue
        lines = body.split("\n")
        i = 0
        while i < len(lines):
            line = lines[i]
            # Crude function-start detector: line ends with '{' or next line does,
            # AND contains '(' (function signature heuristic).
            if "(" in line and (
                "::" in line or line.lstrip().startswith(("static ", "void ", "bool ", "auto ", "auto*", "template "))
            ):
                # Find the enclosing function body via brace counting.
                j = i
                depth = 0
                started = False
                while j < len(lines):
                    for ch in lines[j]:
                        if ch == "{":
                            depth += 1
                            started = True
                        elif ch == "}":
                            depth -= 1
                    if started and depth <= 0:
                        break
                    j += 1
                if j >= len(lines):
                    i += 1
                    continue
                body_slice = "\n".join(lines[i : j + 1])
                has_alloc = _ALLOCATE_PATTERNS.search(body_slice) is not None
                has_wire = _WIRE_PATTERNS.search(body_slice) is not None
                has_exempt = _EXEMPT_PATTERNS.search(body_slice) is not None
                if has_alloc and not (has_wire or has_exempt):
                    # Find the first allocate-like line for reporting.
                    for k in range(i, j + 1):
                        if _ALLOCATE_PATTERNS.search(lines[k]):
                            fails.append(f"{rel}:{k + 1}: allocate without wire or EXEMPT")
                            break
                i = j + 1
            else:
                i += 1
    return fails


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--self-test", action="store_true", help="Run self-test on this linter")
    args = p.parse_args()

    if args.self_test:
        return _self_test()

    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    lp = _read("src/core/lifetime_pin.hh")  # SSOT (module re-exports only)
    _read("CMakeLists.txt")
    build = _read("build.py")
    test = _read("tests/core/test_general_object_pin_coverage_gate.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")

    # AC1 — production paths either wire or EXEMPT (new helper + counters).
    must("wire_general_object_create_pair_or_exempt", "AC1", lp)
    must("general_object_pin_auto_wire_total", "AC1", lp)
    must("general_object_pin_exempt_total", "AC1", lp)
    must("general_object_pin_adopt_site_count", "AC1", lp)
    must("Issue #2709", "AC1", lp)
    must("kGeneralObjectPinAutoWireIssue = 2709", "AC1", lp)

    # AC2 — linter catches new create site without wire/EXEMPT.
    create_files = [
        "src/compiler/evaluator_primitives_eval.cpp",
        "src/compiler/evaluator_primitives_mutate.cpp",
        "src/compiler/evaluator_primitives_query_workspace.cpp",
        "src/compiler/evaluator_eval_flat.cpp",
    ]
    missing = _scan_create_sites_for_missing_wire_or_exempt(create_files)
    if missing:
        for m in missing:
            fails.append(f"AC2: {m}")

    # AC3 — Soft / sandbox=off / Moving off zero extra cost.
    # Regression: the #2665 wire path already gates on pref <= 0 → zero cost.
    # The new helper also gates: only bumps counters, no extra atomic ops.
    must("general_object_pin_auto_wire_total = 0", "AC3", lp)
    must("general_object_pin_exempt_total = 0", "AC3", lp)

    # AC4 — required-mode fail-closed regression (covered by #2665).
    must("g_general_object_pin_required_enforced_total", "AC4", lp)
    must("g_general_object_pin_required_pref.load(std::memory_order_relaxed) > 0", "AC4", lp)

    # AC5 — additive query keys.
    must("general-object-pin-auto-wire-total", "AC5", obs)
    must("general-object-pin-exempt-total", "AC5", obs)
    must("general-object-pin-adopt-site-count", "AC5", obs)
    must("schema-2709", "AC5", obs)
    must("issue-2709", "AC5", obs)

    # AC6 — source-cite + linter + no docs/design/.
    must("check_general_object_pin_auto_wire_2709", "AC6", build)
    must("cmd_general_object_pin_auto_wire_2709_coverage", "AC6", build)
    must("ac2709_1_default_on_helper", "AC6", test)
    must("ac2709_2_linter_catches_missing_wire", "AC6", test)
    must("ac2709_3_soft_zero_cost", "AC6", test)
    must("ac2709_4_required_mode_fail_closed_regression", "AC6", test)
    must("ac2709_5_query_keys_added", "AC6", test)
    must("ac2709_6_source_and_linter", "AC6", test)
    # No docs/design/ per #1655.
    if _read("docs/design/2709-general-object-pin-auto-wire.md"):
        fails.append("AC6: docs/design/2709-* exists (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2709 GeneralObjectPin mandatory coverage — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
