// @category: unit
// @reason: Issue #2654 — language (hash) / hash-set! grow FlatHashTable so
//          N>8 keys are not silently dropped (fixed capacity 8).
//
//   AC1: (hash) with 16 k/v pairs retains all keys
//   AC2: sequential hash-set! 16 times → hash-length 16 + all keys
//   AC3: updates to existing keys still work after growth
//   AC4: source cites #2654 + flat_hash_grow_eval / load factor
//   AC5: gate wiring (cmake + coverage)

#include "test_harness.hpp"

#include "compiler/mutation_concurrency_health.hh"
#include "compiler/observability_metrics.h"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"

#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// ── AC1: (hash …) with 16 pairs ──
static void ac1_hash_literal_16() {
    std::println("\n--- #2654 AC1: (hash) 16 k/v pairs ---");
    CompilerService cs;
    // Build (hash "k0" 0 "k1" 1 ... "k15" 15)
    std::string expr = "(hash";
    for (int i = 0; i < 16; ++i) {
        expr += " \"k" + std::to_string(i) + "\" " + std::to_string(i);
    }
    expr += ")";
    auto setup = cs.eval("(define *h* " + expr + ")");
    CHECK(setup.has_value(), "AC1: define hash");
    auto len = cs.eval("(hash-length *h*)");
    CHECK(len && is_int(*len) && as_int(*len) == 16, "AC1: hash-length 16");
    for (int i = 0; i < 16; ++i) {
        auto r = cs.eval("(hash-ref *h* \"k" + std::to_string(i) + "\" 'missing)");
        CHECK(r && is_int(*r) && as_int(*r) == i, "AC1: key k" + std::to_string(i) + " retained");
    }
}

// ── AC2: sequential hash-set! from empty ──
static void ac2_hash_set_sequential() {
    std::println("\n--- #2654 AC2: sequential hash-set! 16 times ---");
    CompilerService cs;
    CHECK(cs.eval("(define *h* (hash))").has_value(), "AC2: empty hash");
    for (int i = 0; i < 16; ++i) {
        auto r =
            cs.eval("(hash-set! *h* \"k" + std::to_string(i) + "\" " + std::to_string(i) + ")");
        CHECK(r.has_value(), "AC2: set k" + std::to_string(i));
    }
    auto len = cs.eval("(hash-length *h*)");
    CHECK(len && is_int(*len) && as_int(*len) == 16, "AC2: hash-length 16");
    int ok = 0;
    for (int i = 0; i < 16; ++i) {
        auto r = cs.eval("(hash-ref *h* \"k" + std::to_string(i) + "\" 'missing)");
        if (r && is_int(*r) && as_int(*r) == i)
            ++ok;
    }
    CHECK(ok == 16, "AC2: all 16 keys present (ok=" + std::to_string(ok) + ")");
}

// ── AC3: update after growth ──
static void ac3_update_after_growth() {
    std::println("\n--- #2654 AC3: update existing keys after growth ---");
    CompilerService cs;
    CHECK(cs.eval("(define *h* (hash))").has_value(), "AC3: empty");
    for (int i = 0; i < 12; ++i) {
        cs.eval("(hash-set! *h* \"k" + std::to_string(i) + "\" " + std::to_string(i) + ")");
    }
    // Update early key and a mid key
    CHECK(cs.eval("(hash-set! *h* \"k0\" 100)").has_value(), "AC3: update k0");
    CHECK(cs.eval("(hash-set! *h* \"k5\" 105)").has_value(), "AC3: update k5");
    auto r0 = cs.eval("(hash-ref *h* \"k0\" 'missing)");
    CHECK(r0 && is_int(*r0) && as_int(*r0) == 100, "AC3: k0=100");
    auto r5 = cs.eval("(hash-ref *h* \"k5\" 'missing)");
    CHECK(r5 && is_int(*r5) && as_int(*r5) == 105, "AC3: k5=105");
    auto r7 = cs.eval("(hash-ref *h* \"k7\" 'missing)");
    CHECK(r7 && is_int(*r7) && as_int(*r7) == 7, "AC3: k7 still 7");
    auto len = cs.eval("(hash-length *h*)");
    CHECK(len && is_int(*len) && as_int(*len) == 12, "AC3: length still 12 after updates");
}

// ── AC4: source ──
static void ac4_source() {
    std::println("\n--- #2654 AC4: source-cite grow helpers ---");
    const auto vec = read_file("src/compiler/evaluator_primitives_vector.cpp");
    CHECK(vec.find("#2654") != std::string::npos, "AC4: cites #2654");
    CHECK(vec.find("flat_hash_grow_eval") != std::string::npos, "AC4: grow helper");
    CHECK(vec.find("flat_hash_insert_eval") != std::string::npos, "AC4: insert helper");
    CHECK(vec.find("size * 10") != std::string::npos || vec.find("0.7") != std::string::npos ||
              vec.find("capacity * 7") != std::string::npos,
          "AC4: load-factor threshold");
}

// ── AC5: gate wiring ──
static void ac5_gate() {
    std::println("\n--- #2654 AC5: cmake + coverage ---");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_hash_table_grow") != std::string::npos, "AC5: cmake target");
    const auto build = read_file("build.py");
    CHECK(build.find("check_hash_table_grow_2654") != std::string::npos, "AC5: coverage script");
    CHECK(build.find("cmd_hash_table_grow_coverage") != std::string::npos, "AC5: coverage cmd");
}

static void ac3235_1_vector_set_mutates() {
    std::println("\n--- #3235 AC1: vector-set! mutates slot ---");
    CompilerService cs;
    auto r = cs.eval("(begin (define v (vector 1 2 3 4)) (vector-set! v 0 42) (vector-ref v 0))");
    CHECK(r && !is_error(*r) && is_int(*r) && as_int(*r) == 42, "3235 AC1: vector-ref 42");
}

static void ac3235_2_hash_and_pair() {
    std::println("\n--- #3235 AC2: hash-set! + set-car! mutate ---");
    CompilerService cs;
    auto h = cs.eval("(begin (define h (hash 1 2)) (hash-set! h 3 4) (hash-ref h 3))");
    CHECK(h && !is_error(*h) && is_int(*h) && as_int(*h) == 4, "3235 AC2: hash-ref 4");
    auto p = cs.eval("(begin (define xs (cons 1 2)) (set-car! xs 99) (car xs))");
    CHECK(p && !is_error(*p) && is_int(*p) && as_int(*p) == 99, "3235 AC2: car 99");
}

static void ac3235_3_production_guard() {
    std::println("\n--- #3235 AC3: production Restricted + Guard acquire ---");
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
    aura::compiler::MutationConcurrencyHealthSnapshot clean;
    aura::compiler::set_mutation_concurrency_health_admit_snapshot_for_test(clean);
    ::setenv("AURA_PIPELINE_STRICT", "0", 1);
    aura::compiler::typed_audit::apply_production_audit_defaults();
    aura::compiler::security::apply_production_security_defaults();
    CompilerService cs;
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "3235 AC3: metrics");
    const auto acq0 = m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed);
    auto r = cs.eval("(begin (define v (vector 1 2 3 4)) (vector-set! v 0 42) (vector-ref v 0))");
    CHECK(r && !is_error(*r) && is_int(*r) && as_int(*r) == 42, "3235 AC3: production vector-set!");
    CHECK(m->mutation_guard_try_acquire_total.load(std::memory_order_relaxed) > acq0,
          "3235 AC3: try_acquire bumped");
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
}

static void ac3235_4_source() {
    std::println("\n--- #3235 AC4: source-cite lock + PrimCall Guard ---");
    const auto eix = read_file("src/compiler/evaluator.ixx");
    const auto vec = read_file("src/compiler/evaluator_primitives_vector.cpp");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto build = read_file("build.py");
    CHECK(eix.find("maybe_auto_guard_heap_mutate") != std::string::npos, "3235 AC4: PrimCall");
    CHECK(eix.find("Issue #3235") != std::string::npos, "3235 AC4: evaluator cite");
    CHECK(vec.find("alloc_storage_lock_") != std::string::npos, "3235 AC4: vector-set! lock");
    CHECK(mb.find("maybe_auto_guard_heap_mutate") != std::string::npos, "3235 AC4: helper");
    CHECK(build.find("check_container_mutate_guard_3235") != std::string::npos,
          "3235 AC4: build.py");
    CHECK(read_file("docs/design/3235-container-mutate.md").empty(), "3235 AC4: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3235.cpp").empty(), "3235 AC4: no invent");
}

} // namespace

int run_test_hash_table_grow() {
    std::println("=== Issue #2654: language hash table grow ===");
    ac1_hash_literal_16();
    ac2_hash_set_sequential();
    ac3_update_after_growth();
    ac4_source();
    ac5_gate();
    std::println("\n=== Issue #3235: container mutation Guard ===");
    ac3235_1_vector_set_mutates();
    ac3235_2_hash_and_pair();
    ac3235_3_production_guard();
    ac3235_4_source();
    std::println("\n=== #2654/#3235: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hash_table_grow();
}
#endif
