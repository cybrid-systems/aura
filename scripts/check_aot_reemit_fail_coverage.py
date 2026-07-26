#!/usr/bin/env python3
"""check_aot_reemit_fail_coverage.py — Issue #2095 source gate.

Default-LLVM reemit observability: fail counter + env-gated postmortem
keep-failed-.o for offline debug (refine #2016).

AC1: aura_jit_bridge.cpp defines `extern "C" int aura_reemit_keep_fail_enabled(void)`
     that reads `AURA_REEMIT_KEEP_FAIL=1` (or `AURA_REEMIT_KEEP_FAIL_N>0`)
     env vars and returns 1 when enabled, 0 otherwise.
AC2: aura_jit_bridge.cpp defines `extern "C" void aura_reemit_keep_failed_obj(
     const char* obj_path, const char* reason)` that renames the failed
     .o into `/tmp/aura_reemit_failed/` (mkdir -p) for postmortem
     inspection via `llvm-objdump` / `llvm-dis`.
AC3: aura_jit_bridge_stub.cpp provides weak stubs for both symbols so
     light test binaries (no production bridge TU) link cleanly.
AC4: `default_llvm_incremental_emit` (aura_jit_bridge.cpp) bumps
     `aot_incremental_llvm_emit_fail_total` on `!ok` from
     `compile_function_to_object_by_name` and only THEN optionally
     renames via `aura_reemit_keep_failed_obj` (env-gated).
AC5: observability_metrics.h declares
     `aot_incremental_llvm_emit_fail_total` atomic next to the
     pre-existing `aot_incremental_llvm_emit_total`.
AC6: `evaluator_primitives_obs_eval.cpp` exposes the new keys
     (`aot-incremental-llvm-emit-fail-total`,
      `aot-reemit-keep-fail-enabled`,
      `aot-reemit-keep-fail-debug-dir`,
      `schema-2095`/`issue-2095`)
     on `query:aot-stats` AND on the new
     `query:aot-incremental-reemit-stats` primitive (>= 5 fields,
     pipeline-phase lineage).
AC7: tests/compiler/test_aot_incremental_reemit.cpp has AC13a/b/c
     test functions for the fail counter, keep-fail env, and query
     surface respectively.
AC8: linter self-test (--self-test passes).

Rationale (Issue #2095 body):
  Default-LLVM reemit path falls through to skeleton on compile
  failure — success counter under-reports real LLVM work + failed .o
  always removed so offline debug is hard. Add per-reason fail
  counter + env-gated keep-failed-.o for postmortem.

  Default: non-strict (exit 0, prints coverage summary). Use
  --strict to enforce (exit 1 if any AC fails — gate before merge).
"""

from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRIDGE = ROOT / "src" / "compiler" / "aura_jit_bridge.cpp"
BRIDGE_STUB = ROOT / "src" / "compiler" / "aura_jit_bridge_stub.cpp"
BRIDGE_H = ROOT / "src" / "compiler" / "aura_jit_bridge.h"
METRICS = ROOT / "src" / "compiler" / "observability_metrics.h"
OBS_EVAL = ROOT / "src" / "compiler" / "evaluator_primitives_obs_eval.cpp"
TEST = ROOT / "tests" / "compiler" / "test_aot_incremental_reemit.cpp"


def _extract_body(text: str, open_idx: int) -> str:
    """Extract the body of a brace-delimited block (handles nested {})."""
    assert text[open_idx] == "{", f"Expected '{{' at {open_idx}"
    depth = 0
    i = open_idx
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1 : i]
        i += 1
    return ""


def _find_function_body(text: str, signature_regex: str) -> str:
    """Find a function by its signature regex and return its body."""
    m = re.search(signature_regex, text)
    if not m:
        return ""
    open_idx = text.find("{", m.end() - 1)
    if open_idx < 0:
        return ""
    return _extract_body(text, open_idx)


def main() -> int:
    strict = "--strict" in sys.argv
    failures: list[str] = []

    # AC1: aura_reemit_keep_fail_enabled
    if not BRIDGE.exists():
        failures.append("AC1-AC4: src/compiler/aura_jit_bridge.cpp not found")
        bridge = ""
    else:
        bridge = BRIDGE.read_text(encoding="utf-8", errors="replace")

    if bridge:
        # AC1: keep_fail_enabled reads both env vars
        enabled_body = _find_function_body(
            bridge,
            r'extern\s+"C"\s+int\s+aura_reemit_keep_fail_enabled\s*\(\s*void\s*\)',
        )
        if not enabled_body:
            failures.append(
                "AC1: aura_reemit_keep_fail_enabled C-linkage definition not "
                "found in aura_jit_bridge.cpp (env-gated postmortem hook)"
            )
        else:
            if "AURA_REEMIT_KEEP_FAIL" not in enabled_body:
                failures.append("AC1: aura_reemit_keep_fail_enabled body does not read AURA_REEMIT_KEEP_FAIL env var")
            if "AURA_REEMIT_KEEP_FAIL_N" not in enabled_body:
                failures.append(
                    "AC1: aura_reemit_keep_fail_enabled body does not read AURA_REEMIT_KEEP_FAIL_N env var (count gate)"
                )
            if "std::getenv" not in enabled_body:
                failures.append("AC1: aura_reemit_keep_fail_enabled body does not call std::getenv")

        # AC2: keep_failed_obj renames to debug dir
        keep_body = _find_function_body(
            bridge,
            r'extern\s+"C"\s+void\s+aura_reemit_keep_failed_obj\s*\(',
        )
        if not keep_body:
            failures.append(
                "AC2: aura_reemit_keep_failed_obj C-linkage definition not "
                "found in aura_jit_bridge.cpp (.o rename helper)"
            )
        else:
            if "/tmp/aura_reemit_failed" not in keep_body:
                failures.append(
                    "AC2: aura_reemit_keep_failed_obj body does not use the /tmp/aura_reemit_failed debug dir"
                )
            if "std::rename" not in keep_body:
                failures.append(
                    "AC2: aura_reemit_keep_failed_obj body does not call "
                    "std::rename to move failed .o into the debug dir"
                )
            if "::mkdir" not in keep_body and "mkdir" not in keep_body:
                failures.append(
                    "AC2: aura_reemit_keep_failed_obj body does not mkdir the debug dir (first-failure scenario)"
                )

        # AC4: default_llvm_incremental_emit fails path
        emit_body = _find_function_body(
            bridge,
            r"static\s+bool\s+default_llvm_incremental_emit\s*\(",
        )
        if not emit_body:
            failures.append(
                "AC4: default_llvm_incremental_emit definition not found in "
                "aura_jit_bridge.cpp (#2016/#2095 default-LLVM path)"
            )
        else:
            # !ok branch must bump fail counter
            # Look for the bump near the !ok branch.
            ok_branch = re.search(
                r"if\s*\(\s*!ok\s*\)\s*\{(.*?)return\s+false\s*;",
                emit_body,
                re.DOTALL,
            )
            if not ok_branch:
                failures.append(
                    "AC4: default_llvm_incremental_emit does not have an if (!ok) { ... return false; } fail branch"
                )
            else:
                fail_branch = ok_branch.group(1)
                if "aot_incremental_llvm_emit_fail_total" not in fail_branch:
                    failures.append(
                        "AC4: !ok branch in default_llvm_incremental_emit does "
                        "not bump aot_incremental_llvm_emit_fail_total"
                    )
                if "aura_reemit_keep_fail_enabled" not in fail_branch:
                    failures.append(
                        "AC4: !ok branch in default_llvm_incremental_emit does "
                        "not check aura_reemit_keep_fail_enabled (env-gated "
                        "postmortem hook)"
                    )
                if "aura_reemit_keep_failed_obj" not in fail_branch:
                    failures.append(
                        "AC4: !ok branch in default_llvm_incremental_emit does "
                        "not call aura_reemit_keep_failed_obj when env enabled"
                    )
            # success branch must still remove the .o + bump success counter
            if "aot_incremental_llvm_emit_total" not in emit_body:
                failures.append(
                    "AC4: default_llvm_incremental_emit does not bump "
                    "aot_incremental_llvm_emit_total on success path "
                    "(#2016 AC11 regression)"
                )
            # Success path must still call std::remove on the .o
            if not re.search(
                r"std::remove\s*\(\s*obj_path",
                emit_body,
            ):
                failures.append(
                    "AC4: default_llvm_incremental_emit success path does not "
                    "call std::remove(obj_path) — leaked .o in /tmp"
                )

    # AC3: weak stubs in bridge_stub.cpp
    if not BRIDGE_STUB.exists():
        failures.append("AC3: src/compiler/aura_jit_bridge_stub.cpp not found")
    else:
        stub = BRIDGE_STUB.read_text(encoding="utf-8", errors="replace")
        if "aura_reemit_keep_fail_enabled" not in stub:
            failures.append(
                "AC3: aura_jit_bridge_stub.cpp missing weak stub for "
                "aura_reemit_keep_fail_enabled (light-test binary link)"
            )
        if "aura_reemit_keep_failed_obj" not in stub:
            failures.append(
                "AC3: aura_jit_bridge_stub.cpp missing weak stub for "
                "aura_reemit_keep_failed_obj (light-test binary link)"
            )
        # weak stubs should be marked __attribute__((weak))
        if "aura_reemit_keep_fail_enabled" in stub:
            m = re.search(
                r"aura_reemit_keep_fail_enabled[^;]*;",
                stub,
            )
            if m and "__attribute__((weak))" not in stub[max(0, m.start() - 200) : m.end()]:
                failures.append("AC3: aura_reemit_keep_fail_enabled stub is not marked __attribute__((weak))")

    # AC5: observability_metrics.h declares the new counter
    if not METRICS.exists():
        failures.append("AC5: src/compiler/observability_metrics.h not found")
    else:
        metrics = METRICS.read_text(encoding="utf-8", errors="replace")
        if "aot_incremental_llvm_emit_fail_total" not in metrics:
            failures.append(
                "AC5: observability_metrics.h missing "
                "aot_incremental_llvm_emit_fail_total atomic counter "
                "(next to aot_incremental_llvm_emit_total)"
            )
        # Should be next to the success counter (#2016)
        # Use a precise regex to find the atomic declarations (not comments).
        success_m = re.search(
            r"std::atomic<std::uint64_t>\s+aot_incremental_llvm_emit_total\b",
            metrics,
        )
        fail_m = re.search(
            r"std::atomic<std::uint64_t>\s+aot_incremental_llvm_emit_fail_total\b",
            metrics,
        )
        # Fail counter should be within 200 chars of success (paired declaration)
        if success_m and fail_m and abs(fail_m.start() - success_m.start()) > 200:
            failures.append(
                "AC5: aot_incremental_llvm_emit_fail_total is not adjacent "
                "to aot_incremental_llvm_emit_total in observability_metrics.h "
                "(paired counter declaration pattern)"
            )

    # AC6: query surface exposes the new keys
    if not OBS_EVAL.exists():
        failures.append("AC6: src/compiler/evaluator_primitives_obs_eval.cpp not found")
    else:
        obs_eval = OBS_EVAL.read_text(encoding="utf-8", errors="replace")
        if "aot-incremental-llvm-emit-fail-total" not in obs_eval:
            failures.append(
                "AC6: evaluator_primitives_obs_eval.cpp does not expose aot-incremental-llvm-emit-fail-total key"
            )
        if "aot-reemit-keep-fail-enabled" not in obs_eval:
            failures.append("AC6: evaluator_primitives_obs_eval.cpp does not expose aot-reemit-keep-fail-enabled key")
        if "schema-2095" not in obs_eval:
            failures.append("AC6: evaluator_primitives_obs_eval.cpp does not expose schema-2095 lineage key")
        if '"query:aot-incremental-reemit-stats"' not in obs_eval:
            failures.append(
                "AC6: evaluator_primitives_obs_eval.cpp does not register the "
                "query:aot-incremental-reemit-stats primitive (>= 5 fields, "
                "pipeline-phase lineage)"
            )
        # new primitive must have >= 5 keys (success, fail, keep-enabled, debug-dir, lineage)
        new_prim_body = ""
        m = re.search(
            r'"query:aot-incremental-reemit-stats"[^)]*->\s*EvalValue\s*\{(.*?)\}\s*\)\s*;',
            obs_eval,
            re.DOTALL,
        )
        if m:
            new_prim_body = m.group(1)
            kv_count = new_prim_body.count("make_int(") + new_prim_body.count("make_string(")
            if kv_count < 5:
                failures.append(
                    f"AC6: query:aot-incremental-reemit-stats surface exposes "
                    f"only {kv_count} keys; >= 5 required (success + fail + "
                    f"keep-enabled + debug-dir + lineage)"
                )

    # AC7: tests/compiler/test_aot_incremental_reemit.cpp has AC13a/b/c
    if not TEST.exists():
        failures.append("AC7: tests/compiler/test_aot_incremental_reemit.cpp not found")
    else:
        test_text = TEST.read_text(encoding="utf-8", errors="replace")
        if "ac13a_reemit_fail_counter" not in test_text:
            failures.append("AC7: test_aot_incremental_reemit.cpp missing AC13a (fail counter on compile failure)")
        if "ac13b_reemit_keep_fail" not in test_text:
            failures.append(
                "AC7: test_aot_incremental_reemit.cpp missing AC13b (AURA_REEMIT_KEEP_FAIL keeps failed .o)"
            )
        if "ac13c_reemit_query" not in test_text:
            failures.append(
                "AC7: test_aot_incremental_reemit.cpp missing AC13c (query:aot-incremental-reemit-stats surface)"
            )
        # Main should call them
        if "ac13a_reemit_fail_counter()" not in test_text:
            failures.append("AC7: main() does not call ac13a_reemit_fail_counter()")
        if "ac13b_reemit_keep_fail()" not in test_text:
            failures.append("AC7: main() does not call ac13b_reemit_keep_fail()")
        if "ac13c_reemit_query()" not in test_text:
            failures.append("AC7: main() does not call ac13c_reemit_query()")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        if strict:
            return 1
        # Non-strict: print summary but exit 0
        print(
            f"\nNON-STRICT: {len(failures)} issue(s) above (--strict to enforce)",
            file=sys.stderr,
        )
        return 0

    print("OK: all #2095 ACs satisfied (default-LLVM fail counter + env-gated keep-failed-.o + query surface + tests)")
    return 0


def main_strict() -> int:
    """Always-strict variant for self-test fixtures."""
    saved = sys.argv
    try:
        sys.argv = list(saved) + ["--strict"]
        return main()
    finally:
        sys.argv = saved


def self_test() -> int:
    """Self-test: feed good + bad fixtures through the linter."""
    tmp = Path(tempfile.mkdtemp(prefix="check_2095_selftest_"))
    try:
        good_bridge = tmp / "bridge.cpp"
        good_bridge.write_text(
            "#include <cstdlib>\n"
            "#include <format>\n"
            "#include <string>\n"
            "#include <sys/stat.h>\n"
            "#include <cstring>\n"
            "#include <atomic>\n"
            "\n"
            'extern "C" int aura_reemit_keep_fail_enabled(void) {\n'
            '    if (const char* e = std::getenv("AURA_REEMIT_KEEP_FAIL")) {\n'
            "        if (e[0] == '1' || e[0] == 't') return 1;\n"
            "    }\n"
            '    if (const char* n = std::getenv("AURA_REEMIT_KEEP_FAIL_N")) {\n'
            "        if (n[0] != '\\0' && n[0] != '0') return 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
            "\n"
            'extern "C" void aura_reemit_keep_failed_obj(const char* obj_path, const char* reason) {\n'
            "    if (!obj_path) return;\n"
            '    constexpr const char* kDebugDir = "/tmp/aura_reemit_failed";\n'
            "    ::mkdir(kDebugDir, 0755);\n"
            "    static std::atomic<std::uint64_t> keep_seq{0};\n"
            "    const auto seq = keep_seq.fetch_add(1, std::memory_order_relaxed);\n"
            '    std::string dst = std::format("{}/{}_{}_{}.o", kDebugDir, "fn", reason ? reason : "fail", (unsigned long long)seq);\n'
            "    std::rename(obj_path, dst.c_str());\n"
            "}\n"
            "\n"
            "static bool default_llvm_incremental_emit(const char* name, std::uint64_t region) {\n"
            "    if (!name || !*name) return false;\n"
            "    if (region == 2) return false;\n"
            "    if (!g_batch_deopt_jit) return false;\n"
            '    const std::string obj_path = "/tmp/aura_reemit_x.o";\n'
            "    const bool ok = g_batch_deopt_jit->compile_function_to_object_by_name(name, obj_path);\n"
            "    if (!ok) {\n"
            "        if (aot_metrics())\n"
            "            aot_metrics()->aot_incremental_llvm_emit_fail_total.fetch_add(1, std::memory_order_relaxed);\n"
            "        if (aura_reemit_keep_fail_enabled()) {\n"
            '            aura_reemit_keep_failed_obj(obj_path.c_str(), "compile_failed");\n'
            "        } else {\n"
            "            std::remove(obj_path.c_str());\n"
            "        }\n"
            "        return false;\n"
            "    }\n"
            "    std::remove(obj_path.c_str());\n"
            "    if (aot_metrics())\n"
            "        aot_metrics()->aot_incremental_llvm_emit_total.fetch_add(1, std::memory_order_relaxed);\n"
            "    return true;\n"
            "}\n",
            encoding="utf-8",
        )
        good_stub = tmp / "stub.cpp"
        good_stub.write_text(
            'extern "C" __attribute__((weak)) int aura_reemit_keep_fail_enabled(void) { return 0; }\n'
            'extern "C" __attribute__((weak)) void aura_reemit_keep_failed_obj(const char*, const char*) {}\n',
            encoding="utf-8",
        )
        good_metrics = tmp / "metrics.h"
        good_metrics.write_text(
            "// observability_metrics.h fixture\n"
            "std::atomic<std::uint64_t> aot_incremental_llvm_emit_total{0};      // #2016\n"
            "std::atomic<std::uint64_t> aot_incremental_llvm_emit_fail_total{0};  // #2095\n",
            encoding="utf-8",
        )
        good_obs_eval = tmp / "obs_eval.cpp"
        good_obs_eval.write_text(
            "// obs_eval fixture\n"
            "ObservabilityPrims::register_stats_impl(\n"
            '    "query:aot-incremental-reemit-stats", [&ev](const auto&) -> EvalValue {\n'
            "        std::vector<std::pair<std::string, EvalValue>> kv = {\n"
            '            {"aot-incremental-llvm-emit-total", make_int(0)},\n'
            '            {"aot-incremental-llvm-emit-fail-total", make_int(0)},\n'
            '            {"aot-reemit-keep-fail-enabled", make_int(0)},\n'
            '            {"aot-reemit-keep-fail-debug-dir", make_string(0)},\n'
            '            {"aot-incremental-reemit-stats-lineage", make_int(2095)},\n'
            "        };\n"
            "        return EvalValue{};\n"
            "    });\n"
            "// also expose on query:aot-stats:\n"
            '{"aot-incremental-llvm-emit-fail-total", make_int(0)},\n'
            '{"aot-reemit-keep-fail-enabled", make_int(0)},\n'
            '{"schema-2095", make_int(2095)},\n',
            encoding="utf-8",
        )
        good_test = tmp / "test.cpp"
        good_test.write_text(
            "// test fixture\n"
            "static void ac13a_reemit_fail_counter() {}\n"
            "static void ac13b_reemit_keep_fail() {}\n"
            "static void ac13c_reemit_query() {}\n"
            "int main() { ac13a_reemit_fail_counter(); ac13b_reemit_keep_fail(); ac13c_reemit_query(); return 0; }\n",
            encoding="utf-8",
        )

        import check_aot_reemit_fail_coverage as self_mod

        original = {
            "BRIDGE": self_mod.BRIDGE,
            "BRIDGE_STUB": self_mod.BRIDGE_STUB,
            "BRIDGE_H": self_mod.BRIDGE_H,
            "METRICS": self_mod.METRICS,
            "OBS_EVAL": self_mod.OBS_EVAL,
            "TEST": self_mod.TEST,
        }
        try:
            self_mod.BRIDGE = good_bridge
            self_mod.BRIDGE_STUB = good_stub
            self_mod.BRIDGE_H = tmp / "h.h"  # unused in main()
            self_mod.METRICS = good_metrics
            self_mod.OBS_EVAL = good_obs_eval
            self_mod.TEST = good_test
            rc_good = self_mod.main()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_good != 0:
            print(f"SELF-TEST FAIL: known-good mock rejected (rc={rc_good})", file=sys.stderr)
            return 1

        # Bad fixture: fail counter NOT bumped in !ok branch
        bad_bridge = tmp / "bridge_bad.cpp"
        bad_bridge.write_text(
            "#include <cstdlib>\n"
            'extern "C" int aura_reemit_keep_fail_enabled(void) {\n'
            '    if (const char* e = std::getenv("AURA_REEMIT_KEEP_FAIL")) { return 1; }\n'
            '    if (const char* n = std::getenv("AURA_REEMIT_KEEP_FAIL_N")) { return 1; }\n'
            "    return 0;\n"
            "}\n"
            'extern "C" void aura_reemit_keep_failed_obj(const char* obj_path, const char* reason) {\n'
            '    ::mkdir("/tmp/aura_reemit_failed", 0755);\n'
            '    std::rename(obj_path, "/tmp/aura_reemit_failed/x.o");\n'
            "}\n"
            "static bool default_llvm_incremental_emit(const char* name, std::uint64_t region) {\n"
            "    if (!name || !*name) return false;\n"
            "    const bool ok = false;\n"
            "    if (!ok) {\n"
            "        // BAD: missing aot_incremental_llvm_emit_fail_total bump\n"
            "        return false;\n"
            "    }\n"
            "    return true;\n"
            "}\n",
            encoding="utf-8",
        )
        try:
            self_mod.BRIDGE = bad_bridge
            self_mod.BRIDGE_STUB = good_stub
            self_mod.BRIDGE_H = tmp / "h.h"
            self_mod.METRICS = good_metrics
            self_mod.OBS_EVAL = good_obs_eval
            self_mod.TEST = good_test
            rc_bad = self_mod.main() if False else self_mod.main_strict()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad == 0:
            print(
                "SELF-TEST FAIL: known-bad (no fail counter bump) accepted",
                file=sys.stderr,
            )
            return 1

        # Bad fixture: missing query primitive
        bad_obs_eval = tmp / "obs_eval_bad.cpp"
        bad_obs_eval.write_text(
            "// missing query:aot-incremental-reemit-stats\n"
            '{"aot-incremental-llvm-emit-fail-total", make_int(0)},\n'
            '{"aot-reemit-keep-fail-enabled", make_int(0)},\n'
            '{"schema-2095", make_int(2095)},\n',
            encoding="utf-8",
        )
        try:
            self_mod.BRIDGE = good_bridge
            self_mod.BRIDGE_STUB = good_stub
            self_mod.BRIDGE_H = tmp / "h.h"
            self_mod.METRICS = good_metrics
            self_mod.OBS_EVAL = bad_obs_eval
            self_mod.TEST = good_test
            rc_bad2 = self_mod.main() if False else self_mod.main_strict()
        finally:
            for k, v in original.items():
                setattr(self_mod, k, v)
        if rc_bad2 == 0:
            print(
                "SELF-TEST FAIL: known-bad (missing query primitive) accepted",
                file=sys.stderr,
            )
            return 1

        print("SELF-TEST OK: linter accepts good fixture and rejects bad fixtures")
        return 0
    finally:
        import shutil

        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(main())
