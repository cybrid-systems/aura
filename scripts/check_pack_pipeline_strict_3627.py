#!/usr/bin/env python3
# scripts/check_pack_pipeline_strict_3627.py -- Issue #3627 source-cite gate.
#
# AC1: pipeline_policy.hh apply_pipeline_strict_defaults binds the pack:
#      the #if defined(AURA_PRODUCTION_PACK) arm sets Forbidden and never
#      stores Allow; the #else arm keeps the dev split (sandbox=off →
#      Allow, #2213 AC2). Exactly one pack guard in the header (the
#      disposition consult stays pack-agnostic, no second branch model).
# AC2: operator precedence — the AURA_PIPELINE_STRICT env parse precedes
#      the pack guard (operator wins even in the pack binary).
# AC3: non-pack Soft fixture retained —
#      tests/compiler/test_tree_walker_fallback_strict.cpp keeps the
#      #3627 non-pack ACs (sandbox=off → Allow on this face) wired into
#      its runner.
# AC4: pack runtime fixture exists —
#      tests/compiler/test_pack_pipeline_strict.cpp asserts env-unset
#      Forbidden + dev-flag ignore + operator override.
# AC5: CMake binds the same define to both faces — the aura target and
#      the test_pack_pipeline_strict fixture each define
#      AURA_PRODUCTION_PACK=1.
# AC6: no second policy atom (exactly one std::atomic<std::uint32_t> in
#      pipeline_policy.hh), no new query key / metric (observability
#     _metrics.h has no #3627 cite), no docs/design/*3627*, no
#      tests/**/test_issue_3627.cpp.
# AC7: linter registered in build.py.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SRC = "src/compiler/pipeline_policy.hh"
SOFT = "tests/compiler/test_tree_walker_fallback_strict.cpp"
PACK = "tests/compiler/test_pack_pipeline_strict.cpp"
CMAKE = "CMakeLists.txt"
BUILD = "build.py"
OBS = "src/compiler/observability_metrics.h"

LINTER = "check_pack_pipeline_strict_3627"
FN = "apply_pipeline_strict_defaults(bool dev_sandbox_off)"
GUARD = "#if defined(AURA_PRODUCTION_PACK)"
ENV_PARSE = 'std::getenv("AURA_PIPELINE_STRICT")'
CITE = "#3627"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(src: str, soft: str, pack: str, cmake: str, build: str, obs: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — pack binding inside apply_pipeline_strict_defaults.
    pos = src.find(FN)
    if pos == -1:
        fails.append("AC1: apply_pipeline_strict_defaults definition not found")
        return fails
    body = src[pos:]
    i_guard = body.find(GUARD)
    if i_guard == -1:
        fails.append("AC1: pack guard missing from apply_pipeline_strict_defaults")
        return fails
    i_else = body.find("#else", i_guard)
    i_end = body.find("#endif", i_guard)
    if i_else == -1 or i_end == -1 or not (i_guard < i_else < i_end):
        fails.append("AC1: pack guard not closed with #else/#endif in order")
        return fails
    pack_arm = body[i_guard + len(GUARD) : i_else]
    soft_arm = body[i_else:i_end]
    must(
        "TreeWalkerFallbackPolicy::Forbidden",
        "AC1 pack arm binds Forbidden",
        pack_arm,
    )
    must_not(
        "TreeWalkerFallbackPolicy::Allow",
        "AC1 pack arm never stores Allow",
        pack_arm,
    )
    must("dev_sandbox_off", "AC1 non-pack arm keeps dev split", soft_arm)
    must(
        "TreeWalkerFallbackPolicy::Allow",
        "AC1 non-pack arm keeps sandbox=off Allow (#2213 AC2)",
        soft_arm,
    )
    if src.count(GUARD) != 1:
        fails.append("AC1: exactly one pack guard expected in pipeline_policy.hh")

    # AC2 — operator env parse precedes the pack guard.
    i_env = body.find(ENV_PARSE)
    if i_env == -1:
        fails.append("AC2: AURA_PIPELINE_STRICT env parse missing")
    elif i_env > i_guard:
        fails.append("AC2: env parse must precede the pack guard (operator wins)")
    must("AURA_PIPELINE_STRICT", "AC2 env knob documented", src)

    # AC3 — non-pack Soft fixture retained + wired.
    must("ac5_issue_3627_nonpack_binding", "AC3 soft fixture non-pack AC fn", soft)
    must("3627 AC2: non-pack", "AC3 soft fixture Allow assertion", soft)
    must("ac5_issue_3627_nonpack_binding();", "AC3 soft fixture wired into runner", soft)

    # AC4 — pack runtime fixture.
    must("ac3627_pack_env_unset", "AC4 pack fixture env-unset AC", pack)
    must("ac3627_pack_ignores_dev_flag", "AC4 pack fixture dev-flag AC", pack)
    must("ac3627_operator_wins", "AC4 pack fixture operator AC", pack)
    must(CITE, "AC4 pack fixture cites issue", pack)
    must("TreeWalkerFallbackPolicy::Forbidden", "AC4 pack fixture asserts Forbidden", pack)

    # AC5 — same define on both faces.
    must(
        "target_compile_definitions(aura PRIVATE AURA_PRODUCTION_PACK=1)",
        "AC5 aura pack define",
        cmake,
    )
    must(
        "target_compile_definitions(test_pack_pipeline_strict PRIVATE AURA_PRODUCTION_PACK=1)",
        "AC5 pack fixture define",
        cmake,
    )
    must(
        "add_executable(test_pack_pipeline_strict",
        "AC5 pack fixture target declared",
        cmake,
    )

    # AC6 — no second atom / no new metric / no invented artifacts.
    must("g_tree_walker_fallback_policy_atomic", "AC6 existing policy atom retained", src)
    # The single policy atom renders twice (decl + static local inside
    # g_tree_walker_fallback_policy_atomic); a second policy model would
    # push the count past 2.
    if src.count("std::atomic<std::uint32_t>") != 2:
        fails.append("AC6: exactly one policy atomic expected (no second model)")
    must_not(CITE, "AC6 no new metric / observability change", obs)
    design = ROOT / "docs" / "design"
    if design.is_dir():
        hits = sorted(p.name for p in design.glob("*3627*"))
        if hits:
            fails.append(f"AC6: docs/design/*3627* present: {hits}")
    invented = sorted(str(p.relative_to(ROOT)) for p in ROOT.glob("tests/**/test_issue_3627.cpp"))
    if invented:
        fails.append(f"AC6: invented test_issue_3627.cpp present: {invented}")

    # AC7 — build.py registration.
    must(LINTER, "AC7 build.py registration", build)
    return fails


def _self_test() -> int:
    src_ok = (
        "enum class TreeWalkerFallbackPolicy : std::uint8_t {};\n"
        "inline std::atomic<std::uint32_t>& g_tree_walker_fallback_policy_atomic()"
        " noexcept { static std::atomic<std::uint32_t> p{0}; return p; }\n"
        "inline void apply_pipeline_strict_defaults(bool dev_sandbox_off) noexcept {\n"
        '    const char* e = std::getenv("AURA_PIPELINE_STRICT");\n'
        "    if (e && *e) { return; }\n"
        "#if defined(AURA_PRODUCTION_PACK)\n"
        "    set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy::Forbidden);\n"
        "#else\n"
        "    if (dev_sandbox_off)\n"
        "        set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy::Allow);\n"
        "    else\n"
        "        set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy::Forbidden);\n"
        "#endif\n"
        "}\n"
    )
    soft_ok = (
        "static void ac5_issue_3627_nonpack_binding() {\n"
        "    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Allow,\n"
        '          "3627 AC2: non-pack sandbox=off env unset -> Allow");\n'
        "}\n"
        "    ac5_issue_3627_nonpack_binding();\n"
    )
    pack_ok = (
        "void ac3627_pack_env_unset() {}\n"
        "void ac3627_pack_ignores_dev_flag() {}\n"
        "void ac3627_operator_wins() {}\n"
        "// Issue #3627 pack fixture\n"
        '    CHECK(tree_walker_fallback_policy() == TreeWalkerFallbackPolicy::Forbidden, "");\n'
    )
    cmake_ok = (
        "target_compile_definitions(aura PRIVATE AURA_PRODUCTION_PACK=1)\n"
        "add_executable(test_pack_pipeline_strict tests/compiler/test_pack_pipeline_strict.cpp)\n"
        "target_compile_definitions(test_pack_pipeline_strict PRIVATE AURA_PRODUCTION_PACK=1)\n"
    )
    build_ok = 'ROOT / "scripts" / "check_pack_pipeline_strict_3627.py"'
    obs_ok = "struct CompilerMetrics {\n    std::atomic<std::uint64_t> tree_walker_fallback_total{0};\n};"

    good = _rows(src_ok, soft_ok, pack_ok, cmake_ok, build_ok, obs_ok)
    if good:
        print(f"self-test: synthetic-good fixture unexpectedly failed: {good}")
        return 1

    # Bad 1: pack arm stores Allow — AC1 must trip.
    bad1_src = src_ok.replace(
        "#if defined(AURA_PRODUCTION_PACK)\n    set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy::Forbidden);",
        "#if defined(AURA_PRODUCTION_PACK)\n    set_tree_walker_fallback_policy(TreeWalkerFallbackPolicy::Allow);",
    )
    bad = _rows(bad1_src, soft_ok, pack_ok, cmake_ok, build_ok, obs_ok)
    if not any("never stores Allow" in r or "binds Forbidden" in r for r in bad):
        print("self-test: Allow-in-pack fixture did not trip AC1")
        return 1

    # Bad 2: guard stripped — AC1 must trip.
    bad2_src = src_ok.replace("#if defined(AURA_PRODUCTION_PACK)\n", "").replace("#else\n", "").replace("#endif\n", "")
    bad = _rows(bad2_src, soft_ok, pack_ok, cmake_ok, build_ok, obs_ok)
    if not any("pack guard" in r for r in bad):
        print("self-test: stripped guard fixture did not trip AC1")
        return 1

    # Bad 3: soft fixture loses the non-pack AC — AC3 must trip.
    bad = _rows(src_ok, "// non-pack AC removed\n", pack_ok, cmake_ok, build_ok, obs_ok)
    if not any("AC3" in r for r in bad):
        print("self-test: gutted soft fixture did not trip AC3")
        return 1

    print(f"ok {LINTER} self-test")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3627 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any contract row")
    ap.add_argument("--self-test", action="store_true", help="run synthetic fixtures")
    args = ap.parse_args()
    if args.self_test:
        return _self_test()
    rows = _rows(_read(SRC), _read(SOFT), _read(PACK), _read(CMAKE), _read(BUILD), _read(OBS))
    if rows:
        for r in rows:
            print(f"FAIL {LINTER}: {r}")
        return 1
    print(f"ok {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
