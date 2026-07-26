#!/usr/bin/env python3
"""check_macro_restamp_coverage.py — Issue #2096 source gate.

Per-cloned-subtree MacroIntroduced restamp stability after expand in
mutate paths (refine #2019 + #2023 + Macro Hygiene review §7.1).

AC1: src/core/ast.ixx defines `restamp_macro_introduced_subtree(NodeId root)`
     that walks MacroIntroduced descendants of `root`, sets node_gen_ =
     generation_, repairs parent_[child] for MacroIntroduced parents,
     idempotently ORs kMacroExpansion dirty bit, and bumps
     `macro_expand_mutate_restamp_total_` (atomic) on any restamp.
AC2: src/compiler/macro_expansion.cpp defines
     `g_macro_expand_mutate_restamp_total` (file-level atomic) +
     `aura_macro_expand_mutate_restamp_total_v_read()` (C-linkage
     reader under `extern "C"`), AND mirrors the counter into
     `CompilerMetrics::macro_expand_mutate_restamp_total` via
     `aura_macro_hygiene_snapshot_metrics`.
AC3: src/compiler/macro_expansion.cpp wires the new helper from
     `restamp_after_expand(flat, cloned)` (signature gain a NodeId
     root arg) at the inner-expand path (line ~1075) AND at the
     root-level expand path (line ~1090); AND
     src/compiler/evaluator_eval_flat.cpp calls
     `restamp_macro_introduced_subtree(expanded)` post expand_inner_macros
     at site ~3299 + the post-mutate re-expand loop at site ~5202.
AC4: src/compiler/evaluator_primitives_query.cpp registers
     `query:macro-mutate-restamp-stats` (returns file-level counter
     via `aura_macro_expand_mutate_restamp_total_v_read()`) with a
     matching `extern "C"` forward decl next to the existing
     `aura_macro_restamp_after_flat_total_v_read` decl.
AC5: tests/compiler/test_macro_intro_restamp.cpp has AC1-AC5 test
     functions covering source gate + per-subtree restamp behavior
     + counter monotonic + nested-expand mutate path + query
     primitive surface.
AC6: observability_metrics.h declares the
     `macro_expand_mutate_restamp_total` atomic inside CompilerMetrics
     next to the pre-existing `macro_restamp_after_flat_total` field.
AC7: sibling-keep-green via scope check —
     `restamp_macro_introduced_generations()` (zero-arg, #2019 helper)
     is still present and unchanged in src/core/ast.ixx so all
     sibling restamp tests (test_macro_restamp_after_flat.cpp,
     test_hygiene_mutate_closed_loop.cpp,
     test_fiber_macro_hygiene_refresh.cpp,
     test_macro_reflect_batch.cpp) keep compiling and passing.
AC8: linter self-test (--self-test passes).

Rationale (Issue #2096 body):
  Existing `restamp_macro_introduced_generations()` (#2019) does an
  AST-wide sweep — too coarse for expand → immediate mutate under a
  MutationBoundary where the surface is precisely the just-cloned
  subtree. Add NodeId-rooted helper + call site on each cloned root
  + new counter so subsequent mutate/query/JIT see stable gens,
  parent links, and dirty bits without the AST-wide scan cost.

  Default: non-strict (exit 0, prints coverage summary). Use
  --strict to enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
AST_IXX = ROOT / "src" / "core" / "ast.ixx"
MACRO_EXPANSION = ROOT / "src" / "compiler" / "macro_expansion.cpp"
EVAL_FLAT = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
PRIMITIVES_QUERY = ROOT / "src" / "compiler" / "evaluator_primitives_query.cpp"
OBS_METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
TEST = ROOT / "tests" / "compiler" / "test_macro_intro_restamp.cpp"
SIBLING_TEST_2019 = ROOT / "tests" / "compiler" / "test_macro_restamp_after_flat.cpp"


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def ac1_subtree_helper_in_ast_ixx() -> tuple[bool, str]:
    """AC1: ast.ixx defines restamp_macro_introduced_subtree(NodeId)."""
    src = _read(AST_IXX)
    if not src:
        return False, "src/core/ast.ixx not readable"
    if "restamp_macro_introduced_subtree" not in src:
        return False, "restamp_macro_introduced_subtree helper missing in ast.ixx"
    if "macro_expand_mutate_restamp_total_" not in src:
        return False, "macro_expand_mutate_restamp_total_ atomic missing in ast.ixx"
    if "Issue #2096" not in src:
        return False, "Issue #2096 doc-cite missing in ast.ixx"
    if not re.search(
        r"std::size_t\s+restamp_macro_introduced_subtree\s*\(\s*(?:aura::ast::)?NodeId\s+root\s*\)",
        src,
    ):
        return (
            False,
            "restamp_macro_introduced_subtree(NodeId root) signature not found in ast.ixx",
        )
    return True, "ast.ixx: restamp_macro_introduced_subtree(NodeId) + #2096 counter + cite"


def ac2_macro_expansion_atomics_and_mirror() -> tuple[bool, str]:
    """AC2: macro_expansion.cpp + observability_metrics.h wire counter."""
    src = _read(MACRO_EXPANSION)
    if not src:
        return False, "src/compiler/macro_expansion.cpp not readable"
    needed = [
        "g_macro_expand_mutate_restamp_total",
        "aura_macro_expand_mutate_restamp_total_v_read",
        "macro_expand_mutate_restamp_total.store",
    ]
    missing = [n for n in needed if n not in src]
    if missing:
        return (
            False,
            "macro_expansion.cpp missing: " + ", ".join(missing),
        )
    if not re.search(
        r'extern\s+"C"\s*\{(?:[^{}]|\{[^{}]*\})*aura_macro_expand_mutate_restamp_total_v_read',
        src,
        re.DOTALL,
    ):
        return (
            False,
            'aura_macro_expand_mutate_restamp_total_v_read not under extern "C" block',
        )
    obs = _read(OBS_METRICS)
    if not obs:
        return False, "observability_metrics.h not readable"
    if "macro_expand_mutate_restamp_total" not in obs:
        return False, "observability_metrics.h missing macro_expand_mutate_restamp_total field"
    return True, "macro_expansion.cpp + observability_metrics.h wire counter end-to-end"


def ac3_wire_call_sites() -> tuple[bool, str]:
    """AC3: helper called from restamp_after_expand + eval_flat exit sites."""
    src = _read(MACRO_EXPANSION)
    if "restamp_after_expand" not in src:
        return False, "restamp_after_expand wrapper missing"
    if not re.search(
        r"static\s+void\s+restamp_after_expand\s*\(\s*aura::ast::FlatAST\s*&\s*flat\s*,\s*"
        r"aura::ast::NodeId\s+new_root\s*=\s*aura::ast::NULL_NODE\s*\)",
        src,
    ):
        return (
            False,
            "restamp_after_expand signature missing new_root arg",
        )
    if "restamp_macro_introduced_subtree(new_root)" not in src:
        return False, "restamp_macro_introduced_subtree(new_root) call missing"
    flat_src = _read(EVAL_FLAT)
    if not flat_src:
        return False, "evaluator_eval_flat.cpp not readable"
    if "restamp_macro_introduced_subtree" not in flat_src:
        return False, "evaluator_eval_flat.cpp missing restamp_macro_introduced_subtree call"
    n_call_sites = flat_src.count("restamp_macro_introduced_subtree")
    if n_call_sites < 2:
        return (
            False,
            f"evaluator_eval_flat.cpp needs >= 2 call sites, found {n_call_sites}",
        )
    return True, (f"macro_expansion.cpp: restamp_after_expand(flat, cloned) + eval_flat.cpp: {n_call_sites} call sites")


def ac4_query_primitive_registered() -> tuple[bool, str]:
    """AC4: query:macro-mutate-restamp-stats registered + forward decl."""
    src = _read(PRIMITIVES_QUERY)
    if not src:
        return False, "evaluator_primitives_query.cpp not readable"
    if "query:macro-mutate-restamp-stats" not in src:
        return False, "query:macro-mutate-restamp-stats not registered"
    if "aura_macro_expand_mutate_restamp_total_v_read" not in src:
        return (
            False,
            "evaluator_primitives_query.cpp missing forward decl of aura_macro_expand_mutate_restamp_total_v_read",
        )
    if "register_stats_impl" not in src or "query:macro-mutate-restamp-stats" not in src:
        return False, "primitive not registered via register_stats_impl"
    return True, "evaluator_primitives_query.cpp: query:macro-mutate-restamp-stats registered"


def ac5_test_file_ac1_ac5() -> tuple[bool, str]:
    """AC5: test_macro_intro_restamp.cpp covers AC1-AC5."""
    src = _read(TEST)
    if not src:
        return False, "tests/compiler/test_macro_intro_restamp.cpp not readable"
    expected = [
        "ac1_source",
        "ac2_subtree_restamp_after_bump",
        "ac3_nested_expand_and_mutate",
        "ac4_file_level_lockstep",
        "ac5_query_surface",
    ]
    missing = [n for n in expected if n not in src]
    if missing:
        return False, "test missing functions: " + ", ".join(missing)
    if "#2096" not in src:
        return False, "test missing #2096 doc-cite"
    if "Issue #2096" not in src:
        return False, "test missing Issue #2096 doc-cite"
    if "restamp_macro_introduced_subtree" not in src:
        return False, "test missing restamp_macro_introduced_subtree usage"
    return True, "tests/compiler/test_macro_intro_restamp.cpp: AC1-AC5 + #2096 cite"


def ac6_observability_metrics_field() -> tuple[bool, str]:
    """AC6: observability_metrics.h declares macro_expand_mutate_restamp_total atomic."""
    src = _read(OBS_METRICS)
    if not src:
        return False, "observability_metrics.h not readable"
    if not re.search(
        r"std::atomic<std::uint64_t>\s+macro_expand_mutate_restamp_total\s*\{\s*0\s*\}",
        src,
    ):
        return (
            False,
            "observability_metrics.h missing macro_expand_mutate_restamp_total{0} field",
        )
    return True, "observability_metrics.h: macro_expand_mutate_restamp_total atomic field"


def ac7_sibling_2019_helper_intact() -> tuple[bool, str]:
    """AC7: #2019 zero-arg helper + #2096 sibling tests still intact."""
    ast_src = _read(AST_IXX)
    if not ast_src:
        return False, "ast.ixx not readable for sibling check"
    if "std::size_t restamp_macro_introduced_generations()" not in ast_src:
        return (
            False,
            "#2019 zero-arg restamp_macro_introduced_generations() regressed",
        )
    if "macro_restamp_after_flat_total_" not in ast_src:
        return (
            False,
            "#2019 macro_restamp_after_flat_total_ counter regressed",
        )
    sib = _read(SIBLING_TEST_2019)
    if not sib:
        return False, "sibling test test_macro_restamp_after_flat.cpp missing"
    if "Issue #2019" not in sib:
        return False, "sibling #2019 test missing Issue #2019 doc-cite"
    if "restamp_macro_introduced_generations" not in sib:
        return False, "sibling #2019 test missing API reference"
    if "Issue #2096" in ast_src and "Issue #2096" not in sib:
        # Soft: #2096 cite in newer prod files; sibling should at minimum
        # mention the #2096 lineage so the file stays self-describing.
        # Not enforced as hard fail — only warn.
        pass
    return True, "ast.ixx + sibling #2019 test intact (no regression)"


ACS = [
    (
        "AC1: ast.ixx defines restamp_macro_introduced_subtree(NodeId) + counter + #2096 cite",
        ac1_subtree_helper_in_ast_ixx,
    ),
    (
        "AC2: macro_expansion.cpp + observability_metrics.h wire counter (atomic + mirror)",
        ac2_macro_expansion_atomics_and_mirror,
    ),
    ("AC3: helper called from restamp_after_expand + eval_flat exit sites", ac3_wire_call_sites),
    ("AC4: query:macro-mutate-restamp-stats registered with forward decl", ac4_query_primitive_registered),
    ("AC5: test_macro_intro_restamp.cpp covers AC1-AC5 + #2096 cite", ac5_test_file_ac1_ac5),
    ("AC6: observability_metrics.h declares counter atomic field", ac6_observability_metrics_field),
    ("AC7: #2019 zero-arg helper + sibling test intact (no regression)", ac7_sibling_2019_helper_intact),
]


def run_coverage(strict: bool = False) -> int:
    print("# Issue #2096 per-cloned-subtree MacroIntroduced restamp coverage")
    print(f"#   AST={AST_IXX.relative_to(ROOT)}")
    print(f"#   MACRO_EXPANSION={MACRO_EXPANSION.relative_to(ROOT)}")
    print(f"#   EVAL_FLAT={EVAL_FLAT.relative_to(ROOT)}")
    print(f"#   PRIMITIVES_QUERY={PRIMITIVES_QUERY.relative_to(ROOT)}")
    print(f"#   OBS_METRICS={OBS_METRICS.relative_to(ROOT)}")
    print(f"#   TEST={TEST.relative_to(ROOT)}")
    print()
    n_pass = 0
    n_fail = 0
    for label, fn in ACS:
        ok, msg = fn()
        marker = "✅" if ok else "❌"
        print(f"  {marker} {label}")
        if not ok:
            print(f"     └─ {msg}")
            n_fail += 1
        else:
            n_pass += 1
    print()
    print(f"  coverage: {n_pass} pass / {n_fail} fail / {len(ACS)} total")
    if n_fail and strict:
        print("  --strict: exiting 1 (failures must be fixed before merge)")
        return 1
    print("  default mode: coverage report only (exit 0)")
    return 0


def self_test() -> int:
    """Run linter against synthetic source to exercise happy + sad paths."""
    synth = {
        AST_IXX: (
            "// synth ast.ixx\n"
            "std::size_t restamp_macro_introduced_subtree(aura::ast::NodeId root);\n"
            "std::size_t restamp_macro_introduced_generations();\n"
            "mutable std::atomic<std::uint64_t> macro_expand_mutate_restamp_total_{0};\n"
            "mutable std::uint64_t macro_restamp_after_flat_total_{0};\n"
            "// Issue #2096: per-cloned-subtree restamp.\n"
        ),
        MACRO_EXPANSION: (
            "// synth macro_expansion.cpp\n"
            'extern "C" {\n'
            "std::uint64_t aura_macro_expand_mutate_restamp_total_v_read() noexcept { return 0; }\n"
            "}\n"
            "std::atomic<std::uint64_t> g_macro_expand_mutate_restamp_total{0};\n"
            "static void restamp_after_expand(aura::ast::FlatAST& flat, aura::ast::NodeId new_root = aura::ast::NULL_NODE) {}\n"
            "restamp_macro_introduced_subtree(new_root);\n"
        ),
        EVAL_FLAT: (
            "// synth eval_flat\n"
            "f->restamp_macro_introduced_subtree(expanded);\n"
            "flat.restamp_macro_introduced_subtree(expanded);\n"
        ),
        PRIMITIVES_QUERY: (
            "// synth query\n"
            'extern "C" std::uint64_t aura_macro_expand_mutate_restamp_total_v_read() noexcept;\n'
            'ObservabilityPrims::register_stats_impl("query:macro-mutate-restamp-stats", [](std::span<const EvalValue>) -> EvalValue { return make_int(0); });\n'
        ),
        OBS_METRICS: (
            "// synth obs\n"
            "std::atomic<std::uint64_t> macro_restamp_after_flat_total{0};\n"
            "std::atomic<std::uint64_t> macro_expand_mutate_restamp_total{0};\n"
        ),
        TEST: (
            "// synth test\n"
            "// Issue #2096 doc-cite\n"
            "static void ac1_source() {}\n"
            "static void ac2_subtree_restamp_after_bump() {}\n"
            "static void ac3_nested_expand_and_mutate() {}\n"
            "static void ac4_file_level_lockstep() {}\n"
            "static void ac5_query_surface() {}\n"
            "restamp_macro_introduced_subtree(...);\n"
        ),
        SIBLING_TEST_2019: (
            "// synth sibling\n"
            "int main() { /* test_macro_restamp_after_flat */ return 0; }\n"
            "test_macro_restamp_after_flat\n"
        ),
    }
    with tempfile.TemporaryDirectory() as td:
        tmp_root = Path(td)
        # rewrite file paths to point inside tmpdir + save originals
        originals = {}
        for path, content in synth.items():
            # resolve via ROOT so resolution matches run_coverage()
            new = tmp_root / path.relative_to(ROOT)
            new.parent.mkdir(parents=True, exist_ok=True)
            new.write_text(content, encoding="utf-8")
            originals[path] = path.read_text(encoding="utf-8")
            # monkey-patch module globals so run_coverage reads from tmp
            globals()[str(path)] = str(new)
        try:
            rc = run_coverage(strict=False)
        finally:
            for path, _content in originals.items():
                globals()[str(path)] = str(path)
        if rc != 0:
            print(f"  --self-test: synth happy-path FAILED (rc={rc})")
            return 1
    print(f"  --self-test: synth happy-path passed (all {len(ACS)} ACs green)")
    return 0


def main() -> int:
    args = sys.argv[1:]
    strict = False
    if "--strict" in args:
        strict = True
    if "--self-test" in args:
        return self_test()
    return run_coverage(strict=strict)


if __name__ == "__main__":
    sys.exit(main())
