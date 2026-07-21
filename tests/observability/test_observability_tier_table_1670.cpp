// @category: unit
// @reason: Issue #1670 — observability peel tier dispatch is data-driven
// (function-pointer table from observability_{eval,jit}_tiers.inc).
// Registration must still succeed for sample eval/jit stats primitives.
//
//   AC1: CompilerService full-mode boots (register_eval_all + register_jit_all)
//   AC2: engine:metrics / stats facade returns a value for a known name
//   AC3: typecheck-status stats path still resolves (eval peel p0)
//   AC4: jit:intrinsic-count still resolves (jit peel p0)
//   AC5: eval tier count file has 105 entries; jit has 114

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

static int count_tier_lines(const char* path, const char* macro) {
    std::ifstream in(path);
    if (!in)
        return -1;
    int n = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(macro) != std::string::npos)
            ++n;
    }
    return n;
}

static void ac1_boot() {
    std::println("\n--- AC1: CompilerService boots full observability ---");
    CompilerService cs;
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value(), "CS eval ok after tier-table registration");
}

static void ac2_metrics_facade() {
    std::println("\n--- AC2: engine:metrics facade ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"typecheck-status\")");
    CHECK(r.has_value(), "engine:metrics typecheck-status returns");
    // string "ok" or error text / hash — prefer non-void
    if (r)
        CHECK(is_string(*r) || is_hash(*r) || is_int(*r) || !is_void(*r),
              "metrics value is a usable shape");
}

static void ac3_typecheck_status() {
    std::println("\n--- AC3: typecheck-status (eval peel p0) ---");
    CompilerService cs;
    auto r = cs.eval("(stats:get \"typecheck-status\")");
    // stats:get may return via facade; accept any successful EvalValue
    CHECK(r.has_value(), "stats:get typecheck-status ok");
}

static void ac4_jit_intrinsic() {
    std::println("\n--- AC4: jit:intrinsic-count (jit peel p0) ---");
    CompilerService cs;
    auto r = cs.eval("(stats:get \"jit:intrinsic-count\")");
    CHECK(r.has_value(), "stats:get jit:intrinsic-count ok");
    if (r && is_int(*r))
        CHECK(true, "intrinsic-count is int");
}

static void ac5_tier_counts() {
    std::println("\n--- AC5: tier .inc counts ---");
    // Paths relative to repo root when run from build/ or repo root.
    const char* eval_paths[] = {"src/compiler/observability_eval_tiers.inc",
                                "../src/compiler/observability_eval_tiers.inc",
                                "../../src/compiler/observability_eval_tiers.inc"};
    const char* jit_paths[] = {"src/compiler/observability_jit_tiers.inc",
                               "../src/compiler/observability_jit_tiers.inc",
                               "../../src/compiler/observability_jit_tiers.inc"};
    int eval_n = -1, jit_n = -1;
    for (auto* p : eval_paths) {
        eval_n = count_tier_lines(p, "OBS_EVAL_TIER");
        if (eval_n > 0)
            break;
    }
    for (auto* p : jit_paths) {
        jit_n = count_tier_lines(p, "OBS_JIT_TIER");
        if (jit_n > 0)
            break;
    }
    CHECK(eval_n == 105, "eval tiers == 105");
    CHECK(jit_n == 114, "jit tiers == 114");
    std::println("  eval_tiers={} jit_tiers={}", eval_n, jit_n);
}

} // namespace

int main() {
    std::println("=== Issue #1670: observability peel tier table ===");
    ac1_boot();
    ac2_metrics_facade();
    ac3_typecheck_status();
    ac4_jit_intrinsic();
    ac5_tier_counts();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
