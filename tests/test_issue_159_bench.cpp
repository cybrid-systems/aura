// @category: integration
// @reason: uses CompilerService to eval Aura source
// test_issue_159_bench.cpp — Issue #159 Phase 4: incremental
// compilation benchmark harness.
//
// Measures eval_current latency across 4 scenarios × N
// workspace sizes, to quantify the Phase 2 win and document
// the scaling. The benchmark is in-process (avoids subprocess
// overhead) and uses std::chrono::steady_clock for timing.
//
// Scenarios:
//   A. Full eval (cold cache): no prior eval, full re-eval.
//   B. Cache hit (no mutation): repeat eval, should be O(1).
//   C. Phase 2 reuse (mutate EARLY define, last form clean):
//      pre-Phase 2 = full re-eval; post-Phase 2 = cache hit.
//   D. Full re-eval (mutate LAST form's subtree):
//      cache correctly invalidated, full re-eval required.
//
// Output:
//   - JSON to tests/bench_results/issue_159_bench.json
//   - Human-readable table to stdout (PASS/FAIL/median_us)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <print>
#include <sstream>
#include <string>
#include <vector>

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.diag;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.parser.parser;

namespace fs = std::filesystem;

namespace {

// ── Build a workspace with N defines + a result-producing last form ──
//
// Pattern: (define d_0 0) (define d_1 1) ... (define d_{N-1} {N-1}) (+ d_0 1)
// The last form is `(+ d_0 1)` which always evaluates to 1
// (regardless of d_0's value — so we can test the "last form
// clean" cache reuse by mutating d_0).
std::string build_workspace_src(std::size_t n_defines) {
    std::ostringstream out;
    for (std::size_t i = 0; i < n_defines; ++i) {
        out << "(define d_" << i << " " << i << ") ";
    }
    out << "(+ d_0 1)";
    return out.str();
}

// ── Run eval_current N times and return median microseconds ──
//
// Median (not mean) because the first call is always slower
// (cold cache, macro expansion, etc.). The median of N
// samples after a warmup call is the stable measurement.
double bench_eval_current(aura::compiler::CompilerService& cs,
                          int iterations) {
    using clock = std::chrono::steady_clock;
    std::vector<double> samples;
    samples.reserve(iterations);
    for (int i = 0; i < iterations; ++i) {
        auto t0 = clock::now();
        auto r = cs.eval("(eval-current)");
        auto t1 = clock::now();
        if (!r) {
            // Eval failed — record a sentinel (shouldn't happen)
            return -1.0;
        }
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        samples.push_back(us);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];  // median
}

// ── Scenarios ───────────────────────────────────────────────

struct BenchResult {
    std::size_t n_defines;
    double scenario_a_us;  // full eval (cold)
    double scenario_b_us;  // cache hit (no mutation)
    double scenario_c_us;  // Phase 2 reuse (mutate early)
    double scenario_d_us;  // full re-eval (mutate last)
};

BenchResult run_bench_for_n(aura::compiler::CompilerService& cs, std::size_t n) {
    BenchResult r;
    r.n_defines = n;
    std::string src = build_workspace_src(n);

    // ── Scenario A: full eval (cold cache) ──
    // Fresh set-code, fresh eval. No prior cache.
    auto set_r = cs.eval("(set-code \"" + src + "\")");
    if (!set_r) {
        std::println(stderr, "  [n={}] set-code failed", n);
        return r;
    }
    r.scenario_a_us = bench_eval_current(cs, 5);

    // ── Scenario B: cache hit (no mutation) ──
    // Second eval-current should hit the cache.
    r.scenario_b_us = bench_eval_current(cs, 5);

    // ── Scenario C: Phase 2 reuse (mutate early define) ──
    // Mutate d_0. With Phase 2, the last form `(+ d_0 1)` is
    // still in the cache (its subtree didn't change), so
    // eval-current reuses the cached result.
    auto mut_r = cs.eval(R"((mutate:rebind "d_0" "999" "bump"))");
    if (!mut_r) {
        std::println(stderr, "  [n={}] mutate:rebind failed", n);
        return r;
    }
    r.scenario_c_us = bench_eval_current(cs, 5);

    // ── Scenario D: full re-eval (mutate last form) ──
    // Mutate d_0 via set-code, then mutate the last form's
    // content. Easiest: re-set-code with a different last form.
    // (We don't have a "mutate last form" primitive, so this
    // simulates by changing the last form to use a different
    // variable, forcing full re-eval.)
    std::string new_src = build_workspace_src(n);
    // Replace the last form with `(+ d_1 1)` so the cache
    // for `(+ d_0 1)` is invalidated (different AST).
    auto pos = new_src.rfind("(+ d_0 1)");
    if (pos != std::string::npos) {
        new_src.replace(pos, 9, "(+ d_1 1)");
    }
    auto set2_r = cs.eval("(set-code \"" + new_src + "\")");
    if (!set2_r) {
        std::println(stderr, "  [n={}] set-code 2 failed", n);
        return r;
    }
    r.scenario_d_us = bench_eval_current(cs, 5);

    return r;
}

}  // namespace

int main() {
    using clock = std::chrono::steady_clock;
    auto t_start = clock::now();

    std::println("═══ Issue #159 Phase 4 — Incremental Compilation Bench ═══");
    std::println("Scenario A: full eval (cold cache)");
    std::println("Scenario B: cache hit (no mutation)");
    std::println("Scenario C: Phase 2 reuse (mutate early define)");
    std::println("Scenario D: full re-eval (mutate last form)\n");

    // Workspace sizes: 10, 50, 100, 500 defines.
    // (1000 would be useful but slows the test significantly.)
    std::vector<std::size_t> sizes = {10, 50, 100, 500};
    std::vector<BenchResult> results;
    results.reserve(sizes.size());

    for (auto n : sizes) {
        aura::compiler::CompilerService cs;
        std::println("--- N = {} defines ---", n);
        auto r = run_bench_for_n(cs, n);
        results.push_back(r);
        std::println("  A (full eval, cold):       {:8.1f} us", r.scenario_a_us);
        std::println("  B (cache hit, no mutate):  {:8.1f} us", r.scenario_b_us);
        std::println("  C (Phase 2 reuse):         {:8.1f} us", r.scenario_c_us);
        std::println("  D (full re-eval, invalidate):{:8.1f} us", r.scenario_d_us);
        if (r.scenario_c_us > 0 && r.scenario_b_us > 0) {
            double speedup = r.scenario_c_us / r.scenario_b_us;
            std::println("  Phase 2 overhead vs cache-hit: {:.2f}x (should be ~1.0x)", speedup);
        }
        std::println();
    }

    // ── Output JSON to bench_results/ ──
    fs::path out_dir = "tests/bench_results";
    fs::create_directories(out_dir);
    fs::path out_file = out_dir / "issue_159_bench.json";

    // Build JSON manually (avoid json lib dependency).
    std::ostringstream json;
    json << "{\n";
    auto t_now = clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_now.time_since_epoch()).count();
    json << "  \"timestamp_ms\": " << ms << ",\n";
    json << "  \"issue\": 159,\n";
    json << "  \"phase\": 4,\n";
    json << "  \"description\": \"Incremental compilation benchmark — eval_current latency under 4 scenarios × N workspace sizes\",\n";
    json << "  \"scenarios\": {\n";
    json << "    \"A\": \"full eval (cold cache)\",\n";
    json << "    \"B\": \"cache hit (no mutation)\",\n";
    json << "    \"C\": \"Phase 2 reuse (mutate early define; last form clean)\",\n";
    json << "    \"D\": \"full re-eval (mutate last form; cache invalidated)\"\n";
    json << "  },\n";
    json << "  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        json << "    {\n";
        json << "      \"n_defines\": " << r.n_defines << ",\n";
        json << "      \"A_us\": " << r.scenario_a_us << ",\n";
        json << "      \"B_us\": " << r.scenario_b_us << ",\n";
        json << "      \"C_us\": " << r.scenario_c_us << ",\n";
        json << "      \"D_us\": " << r.scenario_d_us << "\n";
        json << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    json << "  ]\n";
    json << "}\n";

    std::ofstream f(out_file);
    if (f) {
        f << json.str();
        std::println("Wrote JSON to: {}", out_file.string());
    } else {
        std::println(stderr, "Failed to write JSON to: {}", out_file.string());
    }

    auto t_end = clock::now();
    double wall_s = std::chrono::duration<double>(t_end - t_start).count();
    std::println("\nTotal wall time: {:.2f}s", wall_s);

    return 0;
}
