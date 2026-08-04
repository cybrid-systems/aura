#!/usr/bin/env python3
"""Issue #2616: hard-ban classify_eval_value_tag (atomics) on eval/IR/apply hot paths.

Contract:
  AC1 zero classify_eval_value_tag in production hot eval/IR/apply sources
  AC2 is_* / as_* route through pure *_hot helpers; AURA_HOT_CONTRACT present
  AC3 query:value-dispatch-stats still uses cold classify counters
  AC4 pure is_fixnum_hot / is_valid_tagged_value have no fetch_add in helpers
  AC5 source-cite #2259/#2616 + test/cmake/build gate; no design docs

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

# Production hot TUs — classify_eval_value_tag (non-consteval) forbidden.
HOT_FILES = [
    "src/compiler/evaluator_eval_flat.cpp",
    "src/compiler/ir_executor_impl.cpp",
    "src/compiler/ir_executor.ixx",
    "src/compiler/value.ixx",
    "src/compiler/aura_jit.cpp",
    "src/compiler/aura_jit_runtime.cpp",
    "src/compiler/lowering_impl.cpp",
    "src/compiler/evaluator_typecheck.cpp",
    "src/compiler/evaluator_mutation_boundary.cpp",
    "src/compiler/evaluator_fiber_mutation.cpp",
    "src/compiler/evaluator_gc.cpp",
    "src/compiler/evaluator_env.cpp",
    "src/compiler/evaluator_ctor.cpp",
    "src/compiler/evaluator_adt.cpp",
]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _strip_comments_and_strings(src: str) -> str:
    # Rough strip for gate (// and /* */ and "...") — not a full C++ lexer.
    out = re.sub(r"//[^\n]*", "", src)
    out = re.sub(r"/\*.*?\*/", "", out, flags=re.S)
    out = re.sub(r'"(?:\\.|[^"\\])*"', '""', out)
    return out


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    tags = _read("src/compiler/value_tags.h")
    value_ixx = _read("src/compiler/value.ixx")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    test = _read("tests/compiler/test_value_tag_hotpath_ban_2616.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # AC1: no classify_eval_value_tag( in hot files (allow consteval + comments already stripped)
    banned = re.compile(r"\bclassify_eval_value_tag\s*\(")
    for rel in HOT_FILES:
        hay = _strip_comments_and_strings(_read(rel))
        if not hay:
            continue
        for m in banned.finditer(hay):
            # Check if this is actually consteval variant
            start = max(0, m.start() - 20)
            window = hay[start : m.end() + 5]
            if "classify_eval_value_tag_consteval" in window:
                continue
            # value.ixx may mention in comments only — comments stripped
            fails.append(f"AC1: {rel} calls classify_eval_value_tag (atomics) near {window!r}")

    must("HOT-PATH BAN", "AC1", tags)
    must("check_value_tag_hotpath_ban_2616", "AC1", tags)
    must("ac1_hot_files_clean", "AC1", test)

    # AC2
    must("is_fixnum_hot", "AC2", value_ixx)
    must("is_string_v2_hot", "AC2", value_ixx)
    must("AURA_HOT_CONTRACT", "AC2", value_ixx)
    must("is_valid_tagged_value_hot", "AC2", tags)
    must("classify_eval_value_tag_consteval", "AC2", tags)
    must("ac2_pure_is_as", "AC2", test)

    # AC3 cold path still available
    must("value_classify_call_count", "AC3", tags)
    must("query:value-dispatch-stats", "AC3", obs)
    must("classify-calls", "AC3", obs)
    must("ac3_cold_agent_stats", "AC3", test)

    # AC4 pure helpers have no fetch_add in their bodies.
    # Match real defs (bool is_fixnum_hot(...)) — not comment mentions of the name
    # (comments can appear earlier next to cold classify_eval_value_tag).
    pure_helpers = (
        "is_fixnum_hot",
        "is_ref_hot",
        "is_string_v2_hot",
        "is_special_hot",
        "is_float_hot",
        "tag_low2_hot",
        "is_valid_tagged_value_hot",
        "is_valid_tagged_value",
    )
    for name in pure_helpers:
        m = re.search(
            rf"bool\s+{re.escape(name)}\s*\([^)]*\)[^{{]*\{{([^}}]+)\}}",
            tags,
            re.S,
        )
        if not m:
            # tag_low2_hot returns uint8_t
            m = re.search(
                rf"(?:bool|std::uint8_t)\s+{re.escape(name)}\s*\([^)]*\)[^{{]*\{{([^}}]+)\}}",
                tags,
                re.S,
            )
        if not m:
            fails.append(f"AC4: missing pure helper def {name}")
            continue
        body = m.group(1)
        if "fetch_add" in body:
            fails.append(f"AC4: pure helper {name} contains fetch_add")
        if "classify_eval_value_tag(" in body and "consteval" not in body:
            fails.append(f"AC4: pure helper {name} still uses atomic classify")
    must("ac4_no_atomic_in_is_hot", "AC4", test)

    # AC5
    must("Issue #2616", "AC5", tags)
    must("#2259", "AC5", tags)
    must("schema-2616", "AC5", obs)
    must("value-tag-hotpath-ban-2616-wired", "AC5", obs)
    must("test_value_tag_hotpath_ban_2616", "AC5", cmake)
    must("check_value_tag_hotpath_ban_2616", "AC5", build)
    must("cmd_value_tag_hotpath_ban_coverage", "AC5", build)
    must("ac5_source_cite", "AC5", test)

    for rel in (
        "docs/design/value_tag_hotpath_ban_2616.md",
        "docs/value_tag_hotpath_ban_2616.md",
        "design/2616.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: unexpected design doc {rel}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2616 value-tag hotpath ban — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
