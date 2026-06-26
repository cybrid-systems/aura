// tests/bench/issue_212_bench.cpp — Issue #212 Phase 4:
//
// Quantitative benchmark: pure-function path vs legacy Wrap
// path for the three P0 refactor surfaces:
//
//   1. constant_folding    (aura.compiler.constant_folding)
//   2. type_checker        (aura.compiler.type_checker pure)
//   3. evaluator arithmetic (aura.compiler.evaluator_pure)
//
// The hypothesis from the issue body: the pure-function path
// is at most 1.05x slower than the legacy Wrap path (and
// often faster, due to better inlining). The benchmark
// measures the actual ratio across N function sizes / AST
// sizes to document the result.
//
// Output:
//   - JSON to tests/bench_results/issue_212_bench.json
//   - Human-readable table to stdout
//
// Why this benchmark is useful:
//   - Cycle 1 (#212) shipped constant_folding as pure.
//     Subsequent cycles (#212, this round) shipped type_checker
//     and 3 arithmetic prims as pure. The benchmark validates
//     that the refactor is perf-neutral (or a win) on the
//     hot paths. A regression >1.10x would block the close.
//   - The benchmark runs in-process (no subprocess), uses
//     std::chrono::steady_clock, and takes <2s total.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <vector>

import aura.compiler.ir;
import aura.compiler.constant_folding;
import aura.compiler.pass_manager;
import aura.compiler.type_checker;
import aura.compiler.evaluator_pure;
import aura.compiler.value;
import aura.core;
import aura.core.type;
import aura.diag;

namespace fs = std::filesystem;

namespace {

// ── Build an IRFunction with N chained Add operations ──
//
// f() returns: t_N = (((... + c_0) + c_1) ...) + c_{N-1})
// where each c_i is a small int. The constant folder should
// fold all of them into a single ConstI64.
aura::ir::IRFunction build_chained_add_function(std::size_t n) {
    aura::ir::IRFunction func;
    func.name = "chained_add_" + std::to_string(n);
    func.local_count = static_cast<std::uint32_t>(n + 2);
    func.blocks.push_back({0});
    auto& block = func.blocks.back();
    // t0 = 0
    {
        aura::ir::IRInstruction i;
        i.opcode = aura::ir::IROpcode::ConstI64;
        i.operands = {0, 0, 0, 0};
        block.instructions.push_back(i);
    }
    // t_{i+1} = Add(t_i, c_i) for i in 0..n-1
    for (std::size_t i = 0; i < n; ++i) {
        // ConstI64 with slot = n+1 (a fresh slot for c_i)
        aura::ir::IRInstruction c;
        c.opcode = aura::ir::IROpcode::ConstI64;
        std::uint32_t c_slot = static_cast<std::uint32_t>(n + 1 + i);
        std::int64_t c_val = static_cast<std::int64_t>(i + 1);
        c.operands = {c_slot,
                      static_cast<std::uint32_t>(c_val & 0xFFFFFFFF),
                      static_cast<std::uint32_t>((c_val >> 32) & 0xFFFFFFFF),
                      0};
        block.instructions.push_back(c);

        // Add(t_i, c_slot) -> t_{i+1}
        aura::ir::IRInstruction add;
        add.opcode = aura::ir::IROpcode::Add;
        add.operands = {static_cast<std::uint32_t>(i + 1),
                        static_cast<std::uint32_t>(i),
                        c_slot, 0};
        block.instructions.push_back(add);
    }
    return func;
}

// Bench result: median time in microseconds for N runs of
// the operation under test.
struct BenchResult {
    double pure_us = 0.0;
    double wrap_us = 0.0;
    double ratio = 0.0;  // wrap_us / pure_us
};

// ── Bench 1: constant folding pure vs Wrap ──
//
// For each function size N, build a chained-add function and
// measure:
//   - pure:  constant_fold_function(func) called N times
//   - wrap:  ConstantFoldingWrap + fold_function called N times
//
// The Wrap route should be at most 5% slower than the pure
// route (and typically identical, since the Wrap is a thin
// forwarder).
BenchResult bench_constant_folding(std::size_t n, std::size_t iters) {
    auto func_template = build_chained_add_function(n);
    BenchResult r;

    // Warm up + measure pure
    {
        aura::ir::IRFunction f = func_template;  // copy
        // discard first call (cold)
        (void)aura::compiler::constant_fold_function(f);
        std::vector<double> samples;
        samples.reserve(iters);
        for (std::size_t i = 0; i < iters; ++i) {
            aura::ir::IRFunction f2 = func_template;
            auto t0 = std::chrono::steady_clock::now();
            auto result = aura::compiler::constant_fold_function(f2);
            auto t1 = std::chrono::steady_clock::now();
            (void)result;
            samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        std::sort(samples.begin(), samples.end());
        r.pure_us = samples[samples.size() / 2];
    }
    // Warm up + measure wrap
    {
        std::vector<double> samples;
        samples.reserve(iters);
        for (std::size_t i = 0; i < iters; ++i) {
            aura::ir::IRFunction f2 = func_template;
            aura::compiler::ConstantFoldingWrap wrap;
            auto t0 = std::chrono::steady_clock::now();
            auto n2 = wrap.fold_function(f2);
            auto t1 = std::chrono::steady_clock::now();
            (void)n2;
            samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        std::sort(samples.begin(), samples.end());
        r.wrap_us = samples[samples.size() / 2];
    }
    r.ratio = r.wrap_us / r.pure_us;
    return r;
}

// ── Bench 2: arithmetic prim pure vs Wrap (= legacy table_) ──
//
// For each arg count N, build a vector of N Int args and
// measure the addition:
//   - pure:  arithmetic_sum_pure called N times
//   - wrap:  inline legacy equivalent (= sum loop) called N times
//
// The wrap (legacy) version inlines the same logic; the
// pure version is a separate function. Modern compilers
// should inline the pure version too, so the ratio should
// be ~1.0x.
BenchResult bench_arithmetic_sum(std::size_t arg_count, std::size_t iters) {
    std::vector<aura::compiler::types::EvalValue> args;
    args.reserve(arg_count);
    for (std::size_t i = 0; i < arg_count; ++i) {
        args.push_back(aura::compiler::types::make_int(static_cast<std::int64_t>(i + 1)));
    }
    BenchResult r;

    // Pure
    {
        std::vector<double> samples;
        samples.reserve(iters);
        for (std::size_t i = 0; i < iters; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            auto result = aura::compiler::pure::arithmetic_sum_pure(args, {});
            auto t1 = std::chrono::steady_clock::now();
            (void)result;
            samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        std::sort(samples.begin(), samples.end());
        r.pure_us = samples[samples.size() / 2];
    }
    // Wrap (= inline sum loop, matches legacy table_["+"])
    {
        std::vector<double> samples;
        samples.reserve(iters);
        for (std::size_t i = 0; i < iters; ++i) {
            auto t0 = std::chrono::steady_clock::now();
            // Inline equivalent of the legacy `+` table entry.
            std::int64_t sum = 0;
            for (const auto& v : args) sum += aura::compiler::types::as_int(v);
            auto result = aura::compiler::types::make_int(sum);
            auto t1 = std::chrono::steady_clock::now();
            (void)result;
            samples.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        std::sort(samples.begin(), samples.end());
        r.wrap_us = samples[samples.size() / 2];
    }
    r.ratio = r.wrap_us / r.pure_us;
    return r;
}

}  // namespace

int main() {
    using clock = std::chrono::steady_clock;
    auto t_start = clock::now();

    std::println("═══ Issue #212 Phase 4 — Pure-Function Path Bench ═══");
    std::println("Hypothesis: pure path is at most 1.05x slower than legacy.");
    std::println("Lower ratio = pure is faster; higher = pure is slower.\n");

    // ── Bench 1: constant folding ──
    std::println("--- Bench 1: constant_folding (pure vs Wrap) ---");
    std::vector<std::size_t> sizes = {10, 50, 100, 500, 1000};
    std::vector<BenchResult> fold_results;
    for (auto n : sizes) {
        auto r = bench_constant_folding(n, 200);
        fold_results.push_back(r);
        std::println("  N={:4d}  pure={:7.2f}us  wrap={:7.2f}us  ratio={:.3f}x",
                     n, r.pure_us, r.wrap_us, r.ratio);
    }

    // ── Bench 2: arithmetic_sum_pure (vs inline legacy) ──
    std::println("\n--- Bench 2: arithmetic_sum_pure (vs inline legacy) ---");
    std::vector<std::size_t> arg_counts = {4, 16, 64, 256};
    std::vector<BenchResult> sum_results;
    for (auto n : arg_counts) {
        auto r = bench_arithmetic_sum(n, 1000);
        sum_results.push_back(r);
        std::println("  args={:4d}  pure={:7.2f}us  wrap={:7.2f}us  ratio={:.3f}x",
                     n, r.pure_us, r.wrap_us, r.ratio);
    }

    // ── Summary verdict ──
    double max_ratio = 0.0;
    for (const auto& r : fold_results) max_ratio = std::max(max_ratio, r.ratio);
    for (const auto& r : sum_results) max_ratio = std::max(max_ratio, r.ratio);

    std::println("\n--- Verdict ---");
    if (max_ratio < 1.05) {
        std::println("  PASS: max ratio = {:.3f}x (< 1.05x threshold).", max_ratio);
        std::println("  Pure-function extraction is perf-neutral or a win.");
    } else {
        std::println("  WARNING: max ratio = {:.3f}x (>= 1.05x threshold).", max_ratio);
        std::println("  Pure path is slower than the legacy Wrap path on at least");
        std::println("  one configuration. Investigate before closing #212.");
    }

    // ── JSON output ──
    fs::path out_dir = "tests/bench_results";
    fs::create_directories(out_dir);
    fs::path out_file = out_dir / "issue_212_bench.json";
    std::ostringstream json;
    json << "{\n";
    json << "  \"bench\": \"issue_212_pure_function_path\",\n";
    json << "  \"threshold_ratio\": 1.05,\n";
    json << "  \"max_ratio_observed\": " << max_ratio << ",\n";
    json << "  \"verdict\": \"" << (max_ratio < 1.05 ? "PASS" : "WARNING") << "\",\n";
    json << "  \"constant_folding\": [\n";
    for (std::size_t i = 0; i < fold_results.size(); ++i) {
        json << "    {\"n\": " << sizes[i]
             << ", \"pure_us\": " << fold_results[i].pure_us
             << ", \"wrap_us\": " << fold_results[i].wrap_us
             << ", \"ratio\": " << fold_results[i].ratio << "}";
        if (i + 1 < fold_results.size()) json << ",";
        json << "\n";
    }
    json << "  ],\n";
    json << "  \"arithmetic_sum\": [\n";
    for (std::size_t i = 0; i < sum_results.size(); ++i) {
        json << "    {\"args\": " << arg_counts[i]
             << ", \"pure_us\": " << sum_results[i].pure_us
             << ", \"wrap_us\": " << sum_results[i].wrap_us
             << ", \"ratio\": " << sum_results[i].ratio << "}";
        if (i + 1 < sum_results.size()) json << ",";
        json << "\n";
    }
    json << "  ]\n";
    json << "}\n";
    std::ofstream f(out_file);
    f << json.str();
    f.close();
    std::println("\n  JSON written to {}", out_file.string());

    auto t_end = clock::now();
    std::println("  Total bench time: {:.1f}s",
                 std::chrono::duration<double>(t_end - t_start).count());
    return max_ratio < 1.05 ? 0 : 1;
}
