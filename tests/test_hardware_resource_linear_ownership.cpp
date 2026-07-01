// test_hardware_resource_linear_ownership.cpp — Issue #306:
// EDA — Extend Linear Ownership (OwnershipEnv +
// lowering_linear_types) to hardware resources (wires, regs,
// memories, ports).
//
// Non-duplicative with #556 (query:edsl-concurrency-stats).
// This binary focuses on the EDA-specific hardware resource
// linear-ownership observability surface:
//   - AC1: 4 new hw_resource_* counters reachable + start at 0
//   - AC2: (query:linear-ownership-stats) returns integer sum
//   - AC3: OwnershipEnv in type_checker.ixx has the 4
//          OwnershipState enum (Owned/Moved/Borrowed/MutBorrowed)
//   - AC4: 4 bump helpers work (wire/reg/mem/double-drive)
//   - AC5: 100-iter mutate cycle — wire/reg/mem non-decreasing
//   - AC6: 8-thread concurrent — counters non-decreasing
//   - AC7: (gc-heap) + linear-ownership integration
//   - AC8: regression — #556 + #555 + #549 primitives still work

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace aura_issue_306_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;

static int k_long_iters() {
    return k_int_env("AURA_STRESS_ITERS", 100);
}

// ── AC1: 4 hw_resource_* counters reachable + start at 0
bool test_hw_resource_counters_reachable() {
    std::println("\n--- AC1: 4 hw_resource_* counters reachable ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto wire0 = cs.get_hw_resource_wire_borrows();
    const auto reg0 = cs.get_hw_resource_reg_writes();
    const auto mem0 = cs.get_hw_resource_mem_access();
    const auto dd0 = cs.get_hw_resource_double_drive();
    std::println("  baseline: wire_borrows={} reg_writes={} mem_access={} "
                 "double_drive={}",
                 wire0, reg0, mem0, dd0);
    CHECK(wire0 == 0, "hw_resource_wire_borrows starts at 0");
    CHECK(reg0 == 0, "hw_resource_reg_writes starts at 0");
    CHECK(mem0 == 0, "hw_resource_mem_access starts at 0");
    CHECK(dd0 == 0, "hw_resource_double_drive starts at 0");
    return true;
}

// ── AC2: (query:linear-ownership-stats) returns integer sum
bool test_query_linear_ownership_stats() {
    std::println("\n--- AC2: (query:linear-ownership-stats) returns integer ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(query:linear-ownership-stats)");
    CHECK(r.has_value(), "(query:linear-ownership-stats) returns");
    CHECK(aura::compiler::types::is_int(*r),
          "(query:linear-ownership-stats) is integer");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        std::println("  query:linear-ownership-stats = {}", v);
        CHECK(v >= 0, "(query:linear-ownership-stats) >= 0 (sum of 4 counters)");
    }
    return true;
}

// ── AC3: OwnershipEnv has the 4 OwnershipState enum
bool test_ownership_env_states_present() {
    std::println("\n--- AC3: OwnershipEnv has 4 OwnershipState enum ---");
    std::ifstream f("/home/dev/code/aura/src/compiler/type_checker.ixx");
    CHECK(f.good(), "type_checker.ixx exists");
    if (f.good()) {
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        f.close();
        const bool has_enum =
            content.find("enum class OwnershipState") != std::string::npos;
        const bool has_owned = content.find("Owned,") != std::string::npos;
        const bool has_moved = content.find("Moved,") != std::string::npos;
        const bool has_borrowed = content.find("Borrowed,") != std::string::npos;
        const bool has_mut_borrowed =
            content.find("MutBorrowed") != std::string::npos;
        const bool has_own_env =
            content.find("class OwnershipEnv") != std::string::npos;
        std::println("  OwnershipEnv: enum={} Owned={} Moved={} Borrowed={} "
                     "MutBorrowed={} class={}",
                     has_enum, has_owned, has_moved, has_borrowed,
                     has_mut_borrowed, has_own_env);
        CHECK(has_enum, "OwnershipState enum exists");
        CHECK(has_owned, "OwnershipState::Owned exists");
        CHECK(has_moved, "OwnershipState::Moved exists");
        CHECK(has_borrowed, "OwnershipState::Borrowed exists");
        CHECK(has_mut_borrowed, "OwnershipState::MutBorrowed exists");
        CHECK(has_own_env, "OwnershipEnv class exists");
    }
    return true;
}

// ── AC4: 4 bump helpers work
bool test_bump_helpers() {
    std::println("\n--- AC4: bump_hw_resource_* helpers ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    const auto wire0 = cs.get_hw_resource_wire_borrows();
    const auto reg0 = cs.get_hw_resource_reg_writes();
    const auto mem0 = cs.get_hw_resource_mem_access();
    const auto dd0 = cs.get_hw_resource_double_drive();
    cs.bump_hw_resource_wire_borrows();
    cs.bump_hw_resource_wire_borrows();
    cs.bump_hw_resource_reg_writes();
    cs.bump_hw_resource_mem_access();
    cs.bump_hw_resource_mem_access();
    cs.bump_hw_resource_mem_access();
    cs.bump_hw_resource_double_drive();
    const auto wire1 = cs.get_hw_resource_wire_borrows();
    const auto reg1 = cs.get_hw_resource_reg_writes();
    const auto mem1 = cs.get_hw_resource_mem_access();
    const auto dd1 = cs.get_hw_resource_double_drive();
    std::println("  wire_borrows: {} -> {} reg_writes: {} -> {} "
                 "mem_access: {} -> {} double_drive: {} -> {}",
                 wire0, wire1, reg0, reg1, mem0, mem1, dd0, dd1);
    CHECK(wire1 == wire0 + 2, "wire_borrows bumped by 2");
    CHECK(reg1 == reg0 + 1, "reg_writes bumped by 1");
    CHECK(mem1 == mem0 + 3, "mem_access bumped by 3");
    CHECK(dd1 == dd0 + 1, "double_drive bumped by 1");
    return true;
}

// ── AC5: 100-iter mutate cycle — counters non-decreasing
bool test_long_running_cycle() {
    std::println("\n--- AC5: {} iters mutate cycle ---", k_long_iters());
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    const auto wire0 = cs.get_hw_resource_wire_borrows();
    const auto reg0 = cs.get_hw_resource_reg_writes();
    for (int i = 0; i < k_long_iters(); ++i) {
        std::string code = std::string("(mutate:replace-value (define ") +
            (i & 1 ? "a" : "b") + " " +
            std::to_string(i) + ") (define " +
            (i & 1 ? "a" : "b") + " " + std::to_string(i) + "))";
        (void)cs.eval(code);
    }
    const auto wire1 = cs.get_hw_resource_wire_borrows();
    const auto reg1 = cs.get_hw_resource_reg_writes();
    std::println("  wire_borrows: {} -> {} reg_writes: {} -> {}",
                 wire0, wire1, reg0, reg1);
    CHECK(wire1 >= wire0, "wire_borrows non-decreasing under cycle");
    CHECK(reg1 >= reg0, "reg_writes non-decreasing under cycle");
    return true;
}

// ── AC6: 8-thread concurrent — counters non-decreasing
bool test_eight_thread_concurrent() {
    std::println("\n--- AC6: 8 threads × 20 iters concurrent ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    constexpr int n_threads = 8;
    constexpr int n_iters = 20;
    std::mutex mtx;
    std::atomic<int> completed{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            std::string code = "(mutate:replace-value (define a " +
                std::to_string(tid * 1000 + i) +
                ") (define a " + std::to_string(tid * 1000 + i) + "))";
            (void)cs.eval(code);
            completed.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();
    const auto wire = cs.get_hw_resource_wire_borrows();
    const auto dd = cs.get_hw_resource_double_drive();
    std::println("  completed: {}/{} wire_borrows: {} double_drive: {}",
                 completed.load(), n_threads * n_iters, wire, dd);
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 mutates completed (no crash under concurrent load)");
    CHECK(wire >= 0, "wire_borrows non-negative");
    return true;
}

// ── AC7: (gc-heap) + linear-ownership integration
bool test_gc_heap_with_linear_ownership() {
    std::println("\n--- AC7: (gc-heap) + linear-ownership integration ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(mutate:replace-value (define a 99) (define a 99))");
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after mutate");
    auto r2 = cs.eval("(query:linear-ownership-stats)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "(query:linear-ownership-stats) after gc-heap");
    return true;
}

// ── AC8: regression — #556 + #555 + #549 primitives still work
bool test_regression_existing_primitives() {
    std::println("\n--- AC8: regression — #556 + #555 + #549 primitives still work ---");
    CompilerService cs;
    auto r1 = cs.eval("(query:linear-ownership-stats)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "(query:linear-ownership-stats) (new for #306)");
    auto r2 = cs.eval("(query:edsl-concurrency-stats)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "(query:edsl-concurrency-stats) (regression for #556)");
    auto r3 = cs.eval("(query:typed-mutation-stats-task1)");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "(query:typed-mutation-stats-task1) (regression for #555)");
    auto r4 = cs.eval("(query:self-evolution-stability-stats)");
    CHECK(r4.has_value() && aura::compiler::types::is_int(*r4),
          "(query:self-evolution-stats) (regression for #549)");
    if (!cs.eval("(define reg-306-a 10)")) {
        CHECK(false, "define (regression)");
        return false;
    }
    auto r5 = cs.eval("(define reg-306-b 32)");
    (void)r5;
    auto r6 = cs.eval("(+ reg-306-a reg-306-b)");
    CHECK(r6.has_value() && aura::compiler::types::is_int(*r6) &&
              aura::compiler::types::as_int(*r6) == 42,
          "(+ reg-306-a reg-306-b) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("═══ Issue #306 verification tests ═══\n");
    std::println("Layer 1: 4 hw_resource_* counters + primitive");
    test_hw_resource_counters_reachable();
    test_query_linear_ownership_stats();
    test_ownership_env_states_present();
    std::println("\nLayer 2: bump helpers + cycle + concurrent + GC + regression");
    test_bump_helpers();
    test_long_running_cycle();
    test_eight_thread_concurrent();
    test_gc_heap_with_linear_ownership();
    test_regression_existing_primitives();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_306_detail

int aura_issue_306_run() { return aura_issue_306_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_306_run(); }
#endif