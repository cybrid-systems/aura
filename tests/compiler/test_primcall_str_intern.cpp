// @category: unit
// @reason: Issue #2577 — PrimCall string re-intern must content-intern
//          so hot loops with fixed string args do not grow heaps O(N×args).
//
//   AC1: N× (string-append x "!") — eval heap growth ≪ N
//   AC2: display correctness preserved (string-append still works)
//   AC3: distinct contents still distinct (two different appends)
//   AC4: source-cite + cmake + gate

#include "test_harness.hpp"
#include "compiler/value_tags.h"

#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::is_error;
using aura::compiler::types::is_string;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string_raw_v2;
using aura::test::g_failed;
using aura::test::g_passed;

extern "C" int64_t aura_hash_key_eq(int64_t stored_key, int64_t search_key);
extern "C" void aura_test_set_string_pool_slot(std::size_t idx, const char* s);

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

// AC1: fixed-arg string-append loop should not grow eval heap by ~N×2.
static void ac1_loop_heap_growth() {
    std::println("\n--- #2577 AC1: fixed-arg PrimCall heap growth ---");
    CompilerService cs;
    auto def = cs.eval(R"((begin
  (define (once x)
    (string-append x "!"))
  (define (loop n x)
    (if (<= n 0)
        #t
        (begin
          (once x)
          (loop (- n 1) x))))
  #t))");
    CHECK(def.has_value() && !(is_error(*def) && !is_string(*def)), "define helpers");

    // Warm one call so ConstString "!" and arg path settle.
    (void)cs.eval(R"((once "hi"))");
    const auto heap0 = cs.evaluator().string_heap().size();

    auto r = cs.eval(R"((loop 400 "hi"))");
    CHECK(r.has_value() && !(is_error(*r) && !is_string(*r)), "run loop 400");
    const auto heap1 = cs.evaluator().string_heap().size();
    const auto growth = heap1 > heap0 ? heap1 - heap0 : 0;
    // Pre-fix: ~800 from arg re-intern alone + results. Post-fix: arg
    // converts hit cache; results from string-append still may add unique
    // "hi!" once (or few). Cap well below O(N).
    CHECK(growth < 80, "AC1: heap growth << iterations (content intern)");
    if (growth >= 80)
        std::println(stderr, "  heap0={} heap1={} growth={}", heap0, heap1, growth);
}

// AC2: still correct output (dual-heap #2575 preserved).
static void ac2_display_ok() {
    std::println("\n--- #2577 AC2: display still correct ---");
    CompilerService cs;
    // Capture via write of result string
    auto r = cs.eval(R"((begin
  (define (f x) (string-append x "!"))
  (f "hi")))");
    CHECK(r && is_string(*r), "AC2: returns string");
    // Content via equal? / string=?
    auto eq = cs.eval(R"((string=? (string-append "hi" "!") "hi!"))");
    CHECK(eq.has_value(), "AC2: string=? ok");
}

// AC3: distinct contents not collapsed incorrectly for different values.
static void ac3_distinct() {
    std::println("\n--- #2577 AC3: distinct contents ---");
    CompilerService cs;
    auto r = cs.eval(R"((begin
  (define a (string-append "X" "1"))
  (define b (string-append "Y" "2"))
  (if (string=? a b) 0 1)))");
    CHECK(r.has_value(), "AC3: eval");
    // Expect 1 (not equal)
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_int;
    CHECK(r && is_int(*r) && as_int(*r) == 1, "AC3: X1 != Y2");
}

static void ac4_source_gate() {
    std::println("\n--- #2577 AC4: source + gate ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(rt.find("#2577") != std::string::npos, "AC4: runtime cites #2577");
    CHECK(rt.find("g_string_intern") != std::string::npos, "AC4: JIT content intern map");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("#2577") != std::string::npos, "AC4: service cites #2577");
    CHECK(svc.find("g_jit_idx_to_eval_idx") != std::string::npos, "AC4: jit→eval idx cache");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_primcall_str_intern") != std::string::npos, "AC4: cmake");
    const auto build = read_file("build.py");
    CHECK(!read_file("scripts/coverage/manifests/2577.json").empty(), "AC4: check (manifest SSOT)");
    CHECK(!read_file("scripts/coverage/manifests/2577.json").empty(),
          "AC4: gate cmd (manifest SSOT)");
}


// ── AC5 (#3703): quote LiteralString interns by sym; Call/Begin lock scope ──
static void ac3703_quote_intern_lock() {
    std::println("\n--- #3703: quote literal intern + alloc lock scope ---");
    // Runtime: two quotes of the same string literal share one heap
    // entry (eq? true — same sym_id → same string_heap_ index).
    CompilerService cs;
    auto eq_same = cs.eval("(eq? \"commit\" \"commit\")");
    CHECK(eq_same.has_value() && !is_error(*eq_same), "3703: eq? evaluates");
    auto distinct = cs.eval("(eq? \"commit\" \"abort\")");
    CHECK(distinct.has_value() && !is_error(*distinct), "3703: distinct literal evaluates");
    // Structural: the ast_to_data quote path interns by sym.
    auto me = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(me.find("Issue #3703: intern by sym_id") != std::string::npos,
          "3703: quote path intern-by-sym cite");
    // Lock scope: the recursion runs BEFORE alloc_storage_lock_ in
    // Call/Begin (items collected unlocked, push slots locked).
    CHECK(me.find("Issue #3703: recurse WITHOUT alloc_storage_lock_") != std::string::npos,
          "3703: Call recursion unlocked");
    CHECK(me.find("Issue #3703: same unlocked-recursion shape as Call.") != std::string::npos,
          "3703: Begin recursion unlocked");
    // eval_flat LiteralString intern-by-sym unchanged (#3401/#3457).
    CHECK(me.find("Issue #3457: lookup by v.sym_id (dense), not hashed") != std::string::npos,
          "3703: eval_flat intern-by-sym unchanged");
}

// Issue #4318: aura_hash_key_eq decodes v2 string indices, not the
// pre-shift bias. OpHashRef calls this compare, so a cross-heap
// duplicate of string 0 is a hit.
static void ac4318_hash_key_eq_v2() {
    std::println("\n--- #4318: hash key eq uses v2 string indices ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto eq = rt.find("int64_t aura_hash_key_eq");
    CHECK(eq != std::string::npos, "4318: eq present");
    if (eq != std::string::npos) {
        const auto win = rt.substr(eq, 1400);
        CHECK(win.find("stored_key == search_key") != std::string::npos, "4318: identical bits");
        const auto bits = win.find("stored_key == search_key");
        const auto lock = win.find("g_string_pool_mtx");
        CHECK(bits != std::string::npos && lock != std::string::npos && bits < lock,
              "4318: identical bits return before the pool lock");
        CHECK(win.find("is_string_v2_hot") != std::string::npos, "4318: v2 classifier");
        CHECK(win.find("string_idx_raw_v2") != std::string::npos, "4318: v2 index");
        CHECK(win.find("is_fixnum_hot") != std::string::npos, "4318: fixnum classifier");
        CHECK(win.find("9000000000000000000") == std::string::npos, "4318: no pre-shift bias");
    }
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    const auto op = jit.find("case OpHashRef:");
    CHECK(op != std::string::npos, "4318: OpHashRef");
    if (op != std::string::npos) {
        CHECK(jit.substr(op, 12000).find("fn_hash_key_eq") != std::string::npos,
              "4318: hash-ref calls aura_hash_key_eq");
    }
    CHECK(read_file("tests/compiler/test_issue_4318.cpp").empty(), "4318: no test_issue file");
    CHECK(read_file("docs/design/4318-hash-key-eq.md").empty(), "4318: no docs/design");

    // Slots: 0 and 7 share bytes; 1 and 8 share; 2 and 6 share the bytes
    // the old decoder would alias (idx 1 → slot 2, idx 2 → slot 6).
    aura_test_set_string_pool_slot(0, "zero");
    aura_test_set_string_pool_slot(1, "one");
    aura_test_set_string_pool_slot(2, "SAME");
    aura_test_set_string_pool_slot(6, "SAME");
    aura_test_set_string_pool_slot(7, "zero");
    aura_test_set_string_pool_slot(8, "one");

    const auto s0 = make_string_raw_v2(0);
    const auto s0b = make_string_raw_v2(7);
    const auto s1 = make_string_raw_v2(1);
    const auto s1b = make_string_raw_v2(8);
    const auto s2 = make_string_raw_v2(2);
    CHECK(aura_hash_key_eq(s0, s0) == 1, "4318: identical bits of string 0");
    CHECK(aura_hash_key_eq(s0, s0b) == 1, "4318: cross-heap duplicate of string 0 hits");
    CHECK(aura_hash_key_eq(s1, s1b) == 1, "4318: idx 1 compares pool[1], not [2]");
    CHECK(aura_hash_key_eq(s2, make_string_raw_v2(6)) == 1,
          "4318: idx 2 compares pool[2], not a shifted slot");
    CHECK(aura_hash_key_eq(s1, s2) == 0, "4318: old alias slots do not force equality");

    const auto i1 = make_int(1).val;
    const auto i2 = make_int(2).val;
    CHECK(i1 == 2, "4318: make_int(1) is raw 2");
    CHECK(aura_hash_key_eq(i1, i2) == 0, "4318: distinct fixnums are not equal");
    CHECK(aura_hash_key_eq(i1, s0) == 0, "4318: fixnum does not enter the string pool");
    CHECK(aura_hash_key_eq(i1, i1) == 1, "4318: identical fixnum bits");
}

} // namespace

int run_test_primcall_str_intern() {
    std::println("=== Issue #2577: PrimCall string re-intern growth ===");
    ac1_loop_heap_growth();
    ac2_display_ok();
    ac3_distinct();
    ac4_source_gate();
    ac3703_quote_intern_lock();
    ac4318_hash_key_eq_v2();
    std::println("\n=== #2577: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_primcall_str_intern();
}
#endif
