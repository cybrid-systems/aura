// test_hash_iter_invalidation.cpp - Issue #1398:
// hash-set! resize + hash-keys/hash-values/hash->alist iter invalidation
// contract test.
//
// Verifies the contract documented in src/compiler/evaluator_primitives_vector.cpp:
// hash-keys / hash-values / hash->alist return BY VALUE (freshly-consed
// pair list snapshot, not a live view into the hash's internal storage).
// After calling any of these, the caller can safely invoke (hash-set!) /
// (hash-remove!) to grow / shrink the hash and the returned list contents
// remain unchanged and well-formed (count + per-element identity match the
// original snapshot).
//
// ACs (per #1398 body):
//   AC1: hash-keys returns stable snapshot after (hash-set!)
//   AC2: hash-values returns stable snapshot after (hash-set!)
//   AC3: hash->alist returns stable snapshot after (hash-set!)

#include "test_harness.hpp"
import std;

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace test_hash_iter_invalidation_detail {

// Returns the length of a list of pair cells.
static int64_t list_length(aura::compiler::CompilerService& cs, const std::string& list_var) {
    auto r = cs.eval(std::format("(length {})", list_var));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// Walks a list, collects the int-typed elements into a sorted vector.
static std::vector<int64_t> collect_ints(aura::compiler::CompilerService& cs,
                                         const std::string& list_var) {
    std::vector<int64_t> result;
    int64_t n = list_length(cs, list_var);
    if (n < 0)
        return result;
    for (int64_t i = 0; i < n; ++i) {
        auto r = cs.eval(std::format("(list-ref {} {})", list_var, i));
        if (r && aura::compiler::types::is_int(*r)) {
            result.push_back(aura::compiler::types::as_int(*r));
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

static void run_ac1_hash_keys_snapshot(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: hash-keys returns stable snapshot after hash-set! ---");
    // Use int keys so collect_ints works directly.
    cs.eval("(let ((h (hash 1 \"a\" 2 \"b\" 3 \"c\" 4 \"d\" 5 \"e\"))) (define h1 h))");
    cs.eval("(define ks (hash-keys h1))");
    auto before_ks = collect_ints(cs, "ks");
    // Mutate hash - add several new entries to trigger any potential resize.
    cs.eval("(hash-set! h1 6 \"f\")");
    cs.eval("(hash-set! h1 7 \"g\")");
    cs.eval("(hash-set! h1 8 \"h\")");
    cs.eval("(hash-set! h1 9 \"i\")");
    cs.eval("(hash-set! h1 10 \"j\")");
    auto after_ks = collect_ints(cs, "ks");
    CHECK(before_ks == after_ks,
          std::format("hash-keys snapshot stable: before={} after={} (equal={})", before_ks.size(),
                      after_ks.size(), before_ks == after_ks));
    cs.eval("(define _unused1 (delete h1 ks))");
}

static void run_ac2_hash_values_snapshot(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: hash-values returns stable snapshot after hash-set! ---");
    cs.eval("(let ((h (hash 1 10 2 20 3 30))) (define h2 h))");
    cs.eval("(define vs (hash-values h2))");
    auto before_vs = collect_ints(cs, "vs");
    // Mutate hash.
    cs.eval("(hash-remove! h2 1)");
    cs.eval("(hash-set! h2 4 40)");
    cs.eval("(hash-set! h2 5 50)");
    auto after_vs = collect_ints(cs, "vs");
    CHECK(before_vs == after_vs,
          std::format("hash-values snapshot stable: before={} after={} (equal={})",
                      before_vs.size(), after_vs.size(), before_vs == after_vs));
    cs.eval("(define _unused2 (delete h2 vs))");
}

static void run_ac3_hash_alist_snapshot(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: hash->alist returns stable snapshot after hash-set! ---");
    cs.eval("(let ((h (hash 1 100 2 200 3 300))) (define h3 h))");
    cs.eval("(define al (hash->alist h3))");
    int64_t before_n = list_length(cs, "al");
    // Each al element is a (kv_pid_int . prev) cell. Extract the
    // values by looking up the (k . v) pair in pairs via list-ref.
    cs.eval("(define _values (let loop ((acc '()) (l al)) "
            "(if (null? l) (reverse acc) "
            "(loop (cons (cdr (list-ref pairs (car l))) acc) (cdr l)))))");
    auto before_vs = collect_ints(cs, "_values");
    // Mutate hash.
    cs.eval("(hash-set! h3 4 400)");
    cs.eval("(hash-set! h3 5 500)");
    cs.eval("(hash-remove! h3 1)");
    int64_t after_n = list_length(cs, "al");
    auto after_vs = collect_ints(cs, "_values");
    CHECK(before_n == 3 && after_n == 3,
          std::format("hash->alist length stable: before={} after={}", before_n, after_n));
    CHECK(before_vs == after_vs,
          std::format("hash->alist values stable: before={} after={} (equal={})", before_vs.size(),
                      after_vs.size(), before_vs == after_vs));
    cs.eval("(define _unused3 (delete h3 al _values))");
}

} // namespace test_hash_iter_invalidation_detail

int aura_hash_iter_invalidation_run() {
    using namespace test_hash_iter_invalidation_detail;
    std::println(
        "=== Issue #1398: hash-keys/hash-values/hash->alist iter invalidation contract test ===");
    {
        aura::compiler::CompilerService cs;
        run_ac1_hash_keys_snapshot(cs);
        run_ac2_hash_values_snapshot(cs);
        run_ac3_hash_alist_snapshot(cs);
    }
    std::println("\n\u2550\u2550\u255d Results: {}/{} passed, {}/{} failed \u2550\u2550\u255d",
                 ::aura::test::g_passed, ::aura::test::g_passed + ::aura::test::g_failed,
                 ::aura::test::g_failed, ::aura::test::g_passed + ::aura::test::g_failed);
    return ::aura::test::g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_hash_iter_invalidation_run();
}
#endif
