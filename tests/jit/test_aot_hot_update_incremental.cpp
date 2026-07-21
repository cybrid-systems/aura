// tests/test_aot_hot_update_incremental.cpp — Issue #1640
//
// AC list (per docs/design/1640-aot-hot-update-incremental.md):
//   AC1: source cites #1640; aot_mangle.h::mangle_aot_name signature
//        extended with env_frame_version + linear_state params.
//   AC2: 2 new metric slots in observability_metrics.h
//        (aot_env_frame_version_drift_prevented +
//         aot_incremental_reemit_triggered).
//   AC3: 2 AURA_COMPILER_METRICS_FIELD(...) entries in
//        compiler_metrics_fields.inc.
//   AC4: 2 bump_/getter pairs in evaluator.ixx.
//   AC5: aura_reload_aot_module_for_eval in aura_jit_bridge.cpp wires
//        env_frame_version drift check (aot_env_frame_version
//        dlsym lookup + drift detection + graceful fallback) +
//        2 metric bumps + rollback_close on drift.
//   AC6: generate_registration_c in aura_jit_bridge.cpp emits
//        aot_env_frame_version + threads env_frame_version +
//        linear_state through mangle_aot_name.
//   AC7: cross-layer regression — CompilerService can be constructed
//        and a basic (set-code) + (eval-current) round-trip still works
//        after the wire-up.
//
// Pattern references: tests/test_incremental_relower_perblock.cpp
// (7 ACs, source-driven), tests/test_aot_hotupdate_versioning.cpp
// (existing AOT hot-update test pattern), tests/test_issue_1638.cpp
// (9 ACs, source-driven).

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_1640_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& s, std::string_view needle) noexcept {
    return s.find(needle) != std::string::npos;
}

bool check_mangle_extension_ac1() {
    std::println("\n--- AC1: mangle_aot_name signature extended ---");
    std::string mangle = read_file("src/compiler/aot_mangle.h");
    bool extended = contains(mangle, "std::uint64_t defuse_version = 0,") &&
                    contains(mangle, "std::uint64_t env_frame_version = 0,") &&
                    contains(mangle, "std::uint8_t linear_state = 0") && contains(mangle, "_e") &&
                    contains(mangle, "_l") && contains(mangle, "Issue #1640");
    if (!extended) {
        std::println(
            "FAIL: mangle_aot_name signature not extended with env_frame_version + linear_state");
        return false;
    }
    std::println("OK: mangle_aot_name extended with env_frame_version + linear_state");
    return true;
}

bool check_metrics_ac2() {
    std::println("\n--- AC2: 2 new metric slots in observability_metrics.h ---");
    std::string om = read_file("src/compiler/observability_metrics.h");
    bool all = contains(om, "aot_env_frame_version_drift_prevented") &&
               contains(om, "aot_incremental_reemit_triggered");
    if (!all) {
        std::println("FAIL: 2 metric slots missing");
        return false;
    }
    std::println("OK: 2 metric slots present");
    return true;
}

bool check_xmacro_ac3() {
    std::println("\n--- AC3: 2 X-macro fields in compiler_metrics_fields.inc ---");
    std::string fields = read_file("src/compiler/compiler_metrics_fields.inc");
    bool all =
        contains(fields, "AURA_COMPILER_METRICS_FIELD(aot_env_frame_version_drift_prevented)") &&
        contains(fields, "AURA_COMPILER_METRICS_FIELD(aot_incremental_reemit_triggered)");
    if (!all) {
        std::println("FAIL: 2 X-macro fields missing");
        return false;
    }
    std::println("OK: 2 X-macro fields present");
    return true;
}

bool check_bump_getter_ac4() {
    std::println("\n--- AC4: 2 bump_/getter pairs in evaluator.ixx ---");
    std::string ixx = read_file("src/compiler/evaluator.ixx");
    bool all = contains(ixx, "bump_aot_env_frame_version_drift_prevented") &&
               contains(ixx, "bump_aot_incremental_reemit_triggered") &&
               contains(ixx, "get_aot_env_frame_version_drift_prevented") &&
               contains(ixx, "get_aot_incremental_reemit_triggered");
    if (!all) {
        std::println("FAIL: missing bump_/getter pair in evaluator.ixx");
        return false;
    }
    std::println("OK: 2 bump_/getter pairs declared");
    return true;
}

bool check_reload_path_ac5() {
    std::println("\n--- AC5: aura_reload_aot_module_for_eval env_frame_version drift check ---");
    std::string bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    bool drift_check = contains(bridge, "Issue #1640: env_frame_version drift detection") &&
                       contains(bridge, "aot_env_frame_version") &&
                       contains(bridge, "binary_env_ver") && contains(bridge, "host_env_ver") &&
                       contains(bridge, "stale env_frame_version") &&
                       contains(bridge, "aot_env_frame_version_drift_prevented.fetch_add") &&
                       contains(bridge, "aot_incremental_reemit_triggered.fetch_add") &&
                       contains(bridge, "rollback_close()") &&
                       contains(bridge, "incremental re-emit triggered, graceful fallback to JIT");
    if (!drift_check) {
        std::println("FAIL: env_frame_version drift check missing or incomplete");
        return false;
    }
    std::println("OK: reload path wires env_frame_version drift check + 2 metric bumps + rollback");
    return true;
}

bool check_generate_registration_ac6() {
    std::println("\n--- AC6: generate_registration_c emits aot_env_frame_version ---");
    std::string bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    bool emits = contains(bridge, "Issue #1640: AOT env_frame_version (captured-env drift)") &&
                 contains(bridge, "const unsigned long long aot_env_frame_version = %llull;") &&
                 contains(bridge, "emit_env_frame_version") &&
                 contains(bridge, "fn_linear_state") && contains(bridge, "linear_ownership_state");
    if (!emits) {
        std::println("FAIL: generate_registration_c does not emit aot_env_frame_version");
        return false;
    }
    std::println("OK: generate_registration_c emits aot_env_frame_version + mangle threading");
    return true;
}

bool check_baseline_ac7(CompilerService& cs) {
    std::println("\n--- AC7: cross-layer baseline round-trip ---");
    if (!cs.eval("(set-code \"(define x 200)\")")) {
        std::println("FAIL: set-code broke");
        return false;
    }
    if (auto r = cs.eval("(eval-current)"); !r || !is_int(*r)) {
        std::println("FAIL: eval-current broke");
        return false;
    }
    std::println("OK: cross-layer baseline round-trip survived #1640 wire-up");
    return true;
}

} // namespace aura_1640_detail

int main() {
    using namespace aura_1640_detail;
    int passed = 0;
    int failed = 0;
    auto run = [&](bool ok) {
        if (ok)
            ++passed;
        else
            ++failed;
        g_passed = passed;
        g_failed = failed;
    };
    std::println("=== Issue #1640: AOT bridge mangle versioning + hot-update observability ===");
    run(check_mangle_extension_ac1());
    run(check_metrics_ac2());
    run(check_xmacro_ac3());
    run(check_bump_getter_ac4());
    run(check_reload_path_ac5());
    run(check_generate_registration_ac6());
    {
        CompilerService cs;
        run(check_baseline_ac7(cs));
    }
    if (failed > 0) {
        std::println("\ntest_aot_hot_update_incremental FAILED ({} passed, {} failed)", passed,
                     failed);
        return 1;
    }
    std::println("\ntest_aot_hot_update_incremental PASS ({} acs, all green)", passed);
    return 0;
}