#!/usr/bin/env python3
"""Issue #4600 source-cite gate (phase 4: §8): lowering is skipped on an
unchanged define.

The #4569 session bench re-runs the same program text in one
--jit-session and showed compiles drop while wall time stayed flat: the
one-line body's LLVM compile is small next to parse, typecheck and
lowering, and those still ran on every exec_jit. This phase adds the
second branch of the §8 acceptance — an unchanged define does not get
re-lowered:

  A session lowering memo holds the post-pipeline IRModule and the
  escape maps of the last program. A re-run of the identical text whose
  own define names (the same set jit_assume_key_ records as
  DefineVersion deps) have not moved reuses it: parse, typecheck,
  lowering, run_production_pipeline and escape analysis are skipped.
  The per-function jit_cache_ probe below still runs, so the #4569
  assumption predicate (IR fingerprint + deps) is unchanged — the memo
  removes re-lowering, not invalidation. `lowering_skips` in
  query:jit-assumption-stats / ;;stats makes the reuse observable.

Two correctness fences ride with it:

  * the memo is dropped on reset() with ir_cache_bridge_ (its IR
    carries no-op-deleter FlatAST/StringPool views from this arena);
  * a service-level recompile of a name whose IR fingerprint actually
    changed evicts AuraJIT's name-keyed compile_fns_ entry first —
    AuraJIT::compile() otherwise returns the PREVIOUS body for that
    name, so an edited define in a second program was invisible in a
    --jit-session (the second program printed the first one's value).
    An epoch-only staleness (AURA_JIT_GLOBAL_EPOCH_INVALIDATE=1) keeps
    the same IR and keeps reusing the native body.

ACs:
  AC1  jit_assumptions.hh: `lowering_skips` in Stats and rendered by
       format_stats (query:jit-assumption-stats / ;;stats visibility).
  AC2  service.ixx §8: JitProgramMemo (input hash + size + strict +
       IRModule + escape maps + DefineVersion deps), the one-entry
       member, jit_reuse_lowering_enabled (AURA_JIT_REUSE_LOWERING=0),
       jit_program_hash_, jit_program_dep_names_ (own name + free vars)
       and jit_program_deps_.
  AC3  service.ixx §8: the memo-hit predicate (hash + size + strict +
       first_broken(deps) == nullptr) bumps lowering_skips and skips
       the parse/typecheck block; the lowering branch reuses
       jit_program_memo_->mod + escape_maps instead of lower_to_ir /
       run_production_pipeline / escape_pass.run.
  AC4  service.ixx §8: the memo is published after the compile loop
       (finalized __lambda__#N names) and cleared in reset(); the
       jit_name_same_ir gate evicts a stale AuraJIT name entry before
       jit_.compile when the IR fingerprint changed.
  AC5  Runtime doors: tests/python/test_regression.py
       test_jit_reuse_lowering_4600 (identical re-run skips lowering and
       stays byte-identical; rebind / changed text / kill switch miss)
       registered in the subprocess list; test_jit_invalidation_walltime_
       4600 asserts lowering_skips and no longer indexes comp[i+1] over
       range(rounds) (it was born red with an IndexError). No
       tests/**/test_issue_4600*.cpp, no docs/design/4600-* (per #1655).
  AC6  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
JA = ROOT / "src" / "compiler" / "jit_assumptions.hh"
SV = ROOT / "src" / "compiler" / "service.ixx"
PYT = ROOT / "tests" / "python" / "test_regression.py"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, label: str) -> None:
    if cond:
        print(f"  ok  {label}")
    else:
        failures.append(label)
        print(f"FAIL  {label}")


def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i + len(start))
    return text[i : j if j > 0 else len(text)]


def main() -> int:
    ja = JA.read_text()
    sv = SV.read_text()
    pyt = PYT.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: stats ────────────────────────────────────────────────
    must("std::atomic<std::uint64_t> lowering_skips{0};" in ja, "AC1: Stats::lowering_skips declared")
    must('kv("lowering_skips"' in ja, "AC1: format_stats renders lowering_skips")

    # ── AC2: memo plumbing ────────────────────────────────────────
    memo = _between(sv, "struct JitProgramMemo {", "};")
    must(bool(memo), "AC2: JitProgramMemo struct present")
    for row, label in (
        ("std::uint64_t input_hash = 0;", "input_hash"),
        ("std::size_t input_size = 0;", "input_size"),
        ("bool strict = false;", "strict"),
        ("aura::ir::IRModule mod;", "IRModule"),
        ("std::vector<std::vector<std::uint8_t>> escape_maps;", "escape_maps"),
        ("std::vector<aura::compiler::jit_assume::Dep> deps;", "deps"),
    ):
        must(row in memo, f"AC2: JitProgramMemo carries {label}")
    must("std::unique_ptr<JitProgramMemo> jit_program_memo_;" in sv, "AC2: one-entry memo member")
    must('std::getenv("AURA_JIT_REUSE_LOWERING")' in sv, "AC2: AURA_JIT_REUSE_LOWERING kill switch")
    must("static std::uint64_t jit_program_hash_(std::string_view s)" in sv, "AC2: jit_program_hash_")
    must(
        'own != "__top__"' in sv and "for (const auto& fv : f.free_vars)" in sv,
        "AC2: jit_program_dep_names_ (own define + free vars)",
    )
    must("jit_program_deps_(const std::vector<std::string>& names)" in sv, "AC2: jit_program_deps_ snapshot")

    # ── AC3: hit predicate + lowering skip ────────────────────────
    hit = _between(sv, "const auto* memo = jit_program_memo_.get();", "if (!memo_hit) {")
    must(bool(hit), "AC3: memo-hit predicate block present")
    for row, label in (
        ("memo->input_size == input.size()", "size compare"),
        ("memo->strict == strict_mode_", "strict-mode compare"),
        ("memo->input_hash == jit_program_hash_(input)", "hash compare"),
        ("first_broken(memo->deps) == nullptr", "DefineVersion deps live"),
        ("stats().lowering_skips.fetch_add", "lowering_skips bump"),
    ):
        must(row in hit, f"AC3: hit predicate has {label}")
    must("if (!memo_hit) {" in sv and "// Parse expression" in sv, "AC3: parse/typecheck run only on a memo miss")
    reuse = _between(sv, "if (memo_hit) {", "} else {")
    must("ir_mod = jit_program_memo_->mod;" in reuse, "AC3: memo hit reuses the post-pipeline IRModule")
    must("escape_pass.maps = jit_program_memo_->escape_maps;" in reuse, "AC3: memo hit reuses the escape maps")
    must("escape_pass.run(ir_mod);" in sv, "AC3: full path still runs escape analysis")

    # ── AC4: publish + reset + stale-name eviction ────────────────
    must(
        "memo->deps = jit_program_deps_(jit_program_dep_names_(ir_mod));" in sv,
        "AC4: memo published after the compile loop with dep snapshot",
    )
    must(
        "memo->mod = ir_mod;" in sv and "memo->escape_maps = escape_pass.maps;" in sv,
        "AC4: publish stores IRModule + escape maps",
    )
    reset = _between(sv, "void reset() {", "ir_cache_bridge_.clear();")
    must("jit_program_memo_.reset();" in reset, "AC4: reset() drops the memo with the arena")
    ev = _between(sv, "Issue #4600 §8 (phase 4): AuraJIT::compile()", "fn_ptr = jit_.compile(builder.flat_fn);")
    must(
        "if (!jit_name_same_ir &&" in ev and "jit_.invalidate(ir_fn.name.c_str());" in ev,
        "AC4: stale AuraJIT name evicted before recompile",
    )
    must(
        "jit_name_same_ir = cache_it->second.fingerprint_ != 0 &&" in sv,
        "AC4: same-IR epoch staleness keeps the native body",
    )
    must("bool jit_name_same_ir = false;" in sv, "AC4: jit_name_same_ir declared per function")

    # ── AC5: doors ────────────────────────────────────────────────
    must("def test_jit_reuse_lowering_4600():" in pyt, "AC5: test_jit_reuse_lowering_4600 present")
    must("    test_jit_reuse_lowering_4600,\n" in pyt, "AC5: reuse-lowering test registered in the subprocess list")
    must("assert skips == [0, 1, 2]" in pyt, "AC5: identical re-run asserts lowering_skips [0,1,2]")
    must("AURA_JIT_REUSE_LOWERING" in pyt, "AC5: python door exercises the kill switch")
    must("assert skips_rb == [0, 0, 1]" in pyt, "AC5: rebound define misses the memo exactly once")
    must("assert skips_c == [0, 0, 0]" in pyt, "AC5: changed text misses the memo")
    must(
        "per_round = [comp_a[i] - comp_a[i - 1] for i in range(1, len(comp_a))]" in pyt,
        "AC5: walltime bench off-by-one fixed",
    )
    must("assert skips_a[-1] >= rounds - 1" in pyt, "AC5: walltime bench asserts the lowering skips")
    must("assert skips_k[-1] == 0" in pyt, "AC5: walltime bench asserts the kill switch")

    # ── AC6: wiring ───────────────────────────────────────────────
    must("check_jit_reuse_lowering_4600.py" in build, "AC6: build.py registers this linter")
    must("check_jit_reuse_lowering_4600.py" in allow, "AC6: root_check_allowlist.txt lists this linter")

    if failures:
        print(f"\n{len(failures)} row(s) failed")
        return 1
    print("\nall rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
