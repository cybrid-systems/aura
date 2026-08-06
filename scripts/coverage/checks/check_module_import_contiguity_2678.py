#!/usr/bin/env python3
"""Issue #2678: runtime(guard) — harden MutationBoundaryGuard against
C++20 module fragility (truncation / dual-def / import contiguity).

Contract:
  AC1 evaluator_mutation_boundary.cpp has a clear ownership boundary
     marker at the top, listing critical-path sections. Prevents
     accidental truncation under partial edits.
  AC2 Bulk restamp / invalidate (restamp_all_pins_for_arena /
     invalidate_all_pins_for_arena / live_pin_count) live ONLY in
     lifetime_pin.ixx. Header declares only; no dual free-function
     definitions. Documented in lifetime_pin.hh.
  AC3 Module import block is the first thing after `module X;` and
     remains contiguous. A simple linter rejects non-contiguous imports.
  AC4 asan-build, ubsan-smoke, deployment-health, reproducible-build
     all stay green (verified by the pre-push gate, not this linter).
  AC5 Existing Guard coverage gates (check_mutation_guard_coverage.py,
     LayoutStamp, densify, occurrence persist, epoch-invariant walk)
     continue to pass without modification of their AC contracts.
  AC6 Zero functional change to mutation semantics, steal safety, or
     GC coordination (no new logic added, just boundary markers).

Exit 0 = all AC rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _check_import_contiguity(file_label: str, hay: str) -> list[str]:
    """Reject any blank line between import statements after `module X;`.

    C++20 module rules require that all imports after `module X;` be
    contiguous — no blank lines, no other statements between imports.
    A blank line or other statement between imports triggers the
    module contiguity rule violation.

    Returns a list of failure strings (empty on pass).
    """
    fails: list[str] = []
    lines = hay.split("\n")
    # Find the `module X;` line (not the GMF `module;`).
    module_purview_line = None
    for i, line in enumerate(lines):
        stripped = line.strip()
        if re.match(r"^module\s+[a-zA-Z_]", stripped) and stripped != "module;":
            module_purview_line = i  # 0-indexed
            break
    if module_purview_line is None:
        return fails  # no module purview, nothing to check
    # Find the import block: starts after `module X;`, ends at first
    # non-import / non-comment line.
    import_block_start = None
    for i in range(module_purview_line + 1, len(lines)):
        stripped = lines[i].strip()
        if stripped.startswith("import "):
            if import_block_start is None:
                import_block_start = i
        elif import_block_start is not None and stripped == "":
            # Blank line inside import block — check if there are more
            # imports after it.
            has_more_imports = False
            for j in range(i + 1, len(lines)):
                if lines[j].strip().startswith("import "):
                    has_more_imports = True
                    break
                if lines[j].strip() != "" and not lines[j].strip().startswith("//"):
                    break
            if has_more_imports:
                fails.append(
                    f"{file_label}: blank line between imports at line {i + 1} "
                    f"(1-indexed) — module import block must be contiguous"
                )
                break
        elif import_block_start is not None and stripped != "" and not stripped.startswith("//"):
            # Non-import, non-comment, non-blank line — end of import block.
            break
    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_count(n: str, label: str, hay: str, at_least: int) -> None:
        c = hay.count(n)
        if c < at_least:
            fails.append(f"{label}: expected ≥{at_least} occurrence(s) of {n!r}, found {c}")

    eval_b = _read("src/compiler/evaluator_mutation_boundary.cpp")
    lpin_hh = _read("src/core/lifetime_pin.hh")
    lpin_ixx = _read("src/core/lifetime_pin.ixx")

    # AC1: ownership boundary marker in evaluator_mutation_boundary.cpp.
    must("OWNERSHIP BOUNDARY", "AC1", eval_b)
    must("Issue #2678", "AC1", eval_b)
    must("try_acquire", "AC1", eval_b)
    must("exit_mutation_boundary", "AC1", eval_b)
    must("Phase-5 densify", "AC1", eval_b)
    must("layout-stamp fence", "AC1", eval_b)
    must("DO NOT split this file", "AC1", eval_b)

    # AC2: bulk restamp/invalidate ONLY in lifetime_pin.ixx.
    # Header must NOT define these free functions (only the comment).
    must("restamp_all_pins_for_arena", "AC2", lpin_ixx)
    must("invalidate_all_pins_for_arena", "AC2", lpin_ixx)
    # Header must document the rule.
    must("Do NOT reintroduce header-form free functions", "AC2", lpin_hh)
    must("Module consumers: import aura.core.lifetime_pin", "AC2", lpin_hh)

    # Reject inline definitions in the header. Use regex to match
    # function definitions (not comments). Must be at line start (not
    # preceded by `// `) and followed by `{` or `noexcept {`.
    def is_definition_in_header(hay: str, name: str) -> bool:
        # Match `inline std::size_t <name>(` or `std::size_t <name>(`
        # followed by `)` and `{` or `noexcept {`. Reject if at line start
        # (not inside a // comment).
        pattern = rf"^[ \t]*(?:inline\s+)?std::size_t\s+{re.escape(name)}\s*\([^)]*\)\s*(?:noexcept)?\s*\{{"
        return bool(re.search(pattern, hay, re.MULTILINE))

    if is_definition_in_header(lpin_hh, "restamp_all_pins_for_arena"):
        fails.append(
            "AC2: lifetime_pin.hh MUST NOT define restamp_all_pins_for_arena "
            "as inline free function (dual-def bug — see #2678)"
        )
    if is_definition_in_header(lpin_hh, "invalidate_all_pins_for_arena"):
        fails.append(
            "AC2: lifetime_pin.hh MUST NOT define invalidate_all_pins_for_arena "
            "as inline free function (dual-def bug — see #2678)"
        )

    # AC3: module import contiguity in evaluator_mutation_boundary.cpp.
    fails.extend(_check_import_contiguity("evaluator_mutation_boundary.cpp", eval_b))
    # Verify the known imports are present.
    must_count("import aura.core.lifetime_pin", "AC3", eval_b, at_least=1)
    must_count("import aura.compiler.coercion_map", "AC3", eval_b, at_least=1)
    must_count("module aura.compiler.evaluator;", "AC3", eval_b, at_least=1)
    must_count("aura.compiler.optimization_passes", "AC3", eval_b, at_least=1)

    # AC3: Generic contiguity check across all .cpp files with module
    # purview (regression gate for future edits).
    import_contiguity_violations = 0
    src_root = ROOT / "src"
    if src_root.is_dir():
        for cpp_path in src_root.rglob("*.cpp"):
            # Skip the OUT files (auto-generated).
            if "/OUT/" in str(cpp_path) or "/out/" in str(cpp_path):
                continue
            try:
                content = cpp_path.read_text(encoding="utf-8", errors="replace")
            except Exception:
                continue
            # Only check files with a module purview declaration.
            if not re.search(r"^module\s+[a-zA-Z_]", content, re.MULTILINE):
                continue
            file_fails = _check_import_contiguity(str(cpp_path.relative_to(ROOT)), content)
            if file_fails:
                import_contiguity_violations += 1
                fails.extend(file_fails)
    if import_contiguity_violations == 0:
        # Print a positive note (visible only on success).
        print("OK: module import contiguity verified across all checked .cpp files")

    # AC5: existing Guard coverage gates still pass (no modification of AC).
    # This is verified at runtime by the pre-push gate running all linters.
    # The linter itself doesn't enforce this — it just records the dependency.
    check_mutation = _read("scripts/coverage/checks/check_mutation_guard_coverage.py")
    must("MutationBoundaryGuard::try_acquire", "AC5", check_mutation)
    must("production", "AC5", check_mutation)

    # AC6: zero functional change — no new logic added, just boundary markers.
    # Verified by the pre-push gate + build.

    # Self-coverage + build.py wire-up.
    build = _read("build.py")
    must("check_module_import_contiguity_2678", "self", build)
    must("#2678", "self", eval_b)
    must("#2678", "self", lpin_hh)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2678 MutationBoundaryGuard C++20 module fragility — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
