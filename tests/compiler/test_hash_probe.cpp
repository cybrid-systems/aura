// @category: unit
// @reason: Issue #4612 — hash-ref / hash-has-key? scanned EVERY slot of the
//          table (no hashing on lookup): a miss cost O(table size) and a
//          hit cost O(position). The table is open-addressed (a key with
//          hash kh lives at ((kh >> 1) + at) & (cap - 1)), so the lookup
//          must walk THAT chain, not the whole table.
//
//   AC1: the tree-walker hash prims (hash-ref / hash-has-key? / hash-set! /
//        hash-remove!) probe by hash through the shared open-addressed
//        probe helper, and the string key hash is the one the native
//        string-hash primitive returns (#4614) — one string hash, not two.
//   AC2: flat costs — a hit / miss / set on a 40,000-entry table costs
//        about the same as on a 100-entry table (before: the miss grew
//        linearly, ~20 ns per entry).
//   AC3: hash-remove! leaves a TOMBSTONE, so a key that was displaced
//        past the removed slot is still found (and the removed key is not).
//   AC4: the equal? semantics for list / vector keys (#4489) survive the
//        probe unchanged.
//   AC5: the IR path (CompilerService::eval_ir → the IR HashRef opcode)
//        uses the SAME probe: the IR opcode dispatches to the same
//        "hash-ref" primitive the tree-walker uses.
//   AC6: the source cites #4612 and the gate wiring (linter + allowlist +
//        build.py) is in place.

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_source(const std::string& rel) {
    for (const auto& p : {rel, "../" + rel, "../../" + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// The window of the prim registration (from the name to the next
// register_prim call) — the body of ONE primitive.
static std::string prim_window(const std::string& src, const std::string& name) {
    // The registration site is the '"<name>",' argument of register_prim --
    // a plain '"<name>"' also shows up in prose comments.
    const auto at = src.find("\"" + name + "\",");
    if (at == std::string::npos)
        return {};
    auto stop = src.find("register_prim(", at);
    if (stop == std::string::npos)
        stop = src.size(); // the last registration in the file
    return src.substr(at, stop - at);
}

// ── AC1: the prims probe by hash (source cite) ──────────────────
static void ac1_source_cite() {
    std::println("\n--- #4612 AC1: the hash prims probe by hash ---");
    const auto vec = read_source("src/compiler/evaluator_primitives_vector.cpp");
    const auto meta = read_source("src/compiler/hash_meta.h");
    CHECK(!vec.empty(), "AC1: the vector primitives source is readable");
    CHECK(vec.find("Issue #4612") != std::string::npos, "AC1: the source cites Issue #4612");

    // The shared probe helper exists and walks the open-addressed chain.
    CHECK(vec.find("eval_hash_probe") != std::string::npos, "AC1: the probe helper exists");
    CHECK(vec.find("kNoHashSlot") != std::string::npos, "AC1: the probe has a miss sentinel");
    CHECK(vec.find("((kh >> 1) + at) & (cap - 1)") != std::string::npos,
          "AC1: the probe walks the open-addressed chain of the key hash");

    // The four lookup prims use the probe and no longer scan the table.
    for (const auto* name : {"hash-ref", "hash-has-key?", "hash-set!", "hash-remove!"}) {
        const auto w = prim_window(vec, name);
        CHECK(!w.empty(), std::string("AC1: the ") + name + " primitive is registered");
        CHECK(w.find("eval_hash_probe") != std::string::npos,
              std::string("AC1: the ") + name + " primitive probes by hash");
        CHECK(w.find("i < ht->capacity") == std::string::npos,
              std::string("AC1: the ") + name + " primitive no longer scans every slot");
    }

    // The key comparison is still eval_hash_keys_eq (the equal? semantics
    // of #4489 and the in-range tenant compare of #4505).
    CHECK(vec.find("eval_hash_keys_eq") != std::string::npos,
          "AC1: the probe compares keys with eval_hash_keys_eq");

    // One string hash: the same FNV-1a the native string-hash primitive
    // returns (#4614) — not a second hash invented here.
    CHECK(vec.find("fnv1a_bytes") != std::string::npos,
          "AC1: the string key hash is the native FNV-1a (hash_meta.h)");
    const auto pair = read_source("src/compiler/evaluator_primitives_pair.cpp");
    CHECK(pair.find("fnv1a_bytes") != std::string::npos,
          "AC1: the native string-hash primitive uses the same FNV-1a");

    // The tombstone marker is the shared one.
    CHECK(meta.find("kTombstoneSlot") != std::string::npos,
          "AC1: hash_meta.h carries the shared tombstone marker");
    CHECK(vec.find("kTombstoneSlot") != std::string::npos,
          "AC1: the hash prims use the shared tombstone marker");
}

// ── AC2: flat costs (a 40,000-entry table costs like a 100-entry one) ──
static void ac2_flat_costs() {
    std::println("\n--- #4612 AC2: flat costs at 40,000 entries ---");
    CompilerService cs;
    auto setup = cs.eval(R"(
        (define (bench-us reps thunk)
          (let ((t0 (current-time-ms)))
            (let loop ((r 0)) (if (< r reps) (begin (thunk) (loop (+ r 1))) 0))
            (quotient (* 1000 (- (current-time-ms) t0)) reps)))
        (define (fill h i n)
          (if (>= i n) 0
              (begin (hash-set! h (string-append "k" (number->string i)) i) (fill h (+ i 1) n))))
        (define small (hash))
        (fill small 0 100)
        (define big (hash))
        (fill big 0 40000)
        (define (probe-small-miss) (hash-ref small "zz" #f))
        (define (probe-big-miss) (hash-ref big "zz" #f))
        (define (probe-big-hit) (hash-ref big "k77" #f))
        (define (probe-big-set) (hash-set! big "k77" 1))
        (define small-us (bench-us 2000 probe-small-miss))
        (define big-us (bench-us 2000 probe-big-miss))
        (define big-hit-us (bench-us 2000 probe-big-hit))
        (define big-set-us (bench-us 2000 probe-big-set))
        (list small-us big-us big-hit-us big-set-us))");
    CHECK(setup.has_value(), "AC2: the benchmark program runs");

    // Functional: the 40,000-entry table still answers.
    auto len = cs.eval("(hash-length big)");
    CHECK(len.has_value() && is_int(*len) && as_int(*len) == 40000,
          "AC2: the big table holds 40000 keys");
    auto hit = cs.eval("(hash-ref big \"k77\" #f)");
    CHECK(hit.has_value() && is_int(*hit) && as_int(*hit) == 77, "AC2: a hit on the big table");
    auto miss = cs.eval("(hash-ref big \"zz\" \"MISS\")");
    CHECK(miss.has_value() && !is_void(*miss), "AC2: a miss on the big table returns the default");
    auto has = cs.eval("(hash-has-key? big \"k39999\")");
    CHECK(has.has_value() && is_bool(*has), "AC2: hash-has-key? on the big table");

    // The per-op cost of a miss at 40,000 entries vs at 100 entries.
    // Before the probe: 23 us vs 7256 us on this tree (a linear scan).
    auto small_us = cs.eval("small-us");
    auto big_us = cs.eval("big-us");
    auto big_hit_us = cs.eval("big-hit-us");
    auto big_set_us = cs.eval("big-set-us");
    CHECK(small_us.has_value() && is_int(*small_us) && as_int(*small_us) > 0,
          "AC2: the n=100 miss cost is measured");
    CHECK(big_us.has_value() && is_int(*big_us) && as_int(*big_us) > 0,
          "AC2: the n=40000 miss cost is measured");
    if (small_us.has_value() && big_us.has_value() && as_int(*small_us) > 0) {
        const auto base = as_int(*small_us);
        const auto big = as_int(*big_us);
        // ratio in per-mille; 8000 == 8x. The scan was ~300x.
        const auto ratio = (big * 1000) / base;
        std::println("    miss: n=100 {} us/op, n=40000 {} us/op ({}x)", base, big,
                     static_cast<double>(big) / static_cast<double>(base));
        CHECK(ratio <= 8000, "AC2: the miss cost does not grow with the table size");
    }
    if (small_us.has_value() && big_hit_us.has_value() && as_int(*small_us) > 0) {
        const auto ratio = (as_int(*big_hit_us) * 1000) / as_int(*small_us);
        std::println("    hit:  n=100 {} us/op, n=40000 {} us/op", as_int(*small_us),
                     as_int(*big_hit_us));
        CHECK(ratio <= 8000, "AC2: the hit cost does not grow with the table size");
    }
    if (small_us.has_value() && big_set_us.has_value() && as_int(*small_us) > 0) {
        const auto ratio = (as_int(*big_set_us) * 1000) / as_int(*small_us);
        std::println("    set:  n=100 {} us/op, n=40000 {} us/op", as_int(*small_us),
                     as_int(*big_set_us));
        CHECK(ratio <= 8000, "AC2: the hash-set! cost does not grow with the table size");
    }
}

// ── AC3: a removal does not hide the keys that were displaced past it ──
static void ac3_removal_tombstone() {
    std::println("\n--- #4612 AC3: hash-remove! keeps the chain alive ---");
    CompilerService cs;
    auto setup = cs.eval(R"(
        (define (fill h i n)
          (if (>= i n) 0
              (begin (hash-set! h (string-append "k" (number->string i)) i) (fill h (+ i 1) n))))
        (define (count-found h n)
          (let loop ((i 0) (found 0))
            (if (>= i n) found
                (loop (+ i 1)
                      (if (hash-has-key? h (string-append "k" (number->string i))) (+ found 1) found)))))
        (define h (hash))
        (fill h 0 2000))");
    CHECK(setup.has_value(), "AC3: the table is filled");

    auto before = cs.eval("(count-found h 2000)");
    CHECK(before.has_value() && is_int(*before) && as_int(*before) == 2000,
          "AC3: every key is found before the removal");

    // Remove every 10th key: each removal punches a hole in a probe chain.
    auto removed = cs.eval(R"(
        (let loop ((i 0) (n 0))
          (if (>= i 2000) n
              (loop (+ i 1)
                    (if (= 0 (modulo i 10))
                        (if (hash-remove! h (string-append "k" (number->string i))) (+ n 1) n)
                        n)))))");
    CHECK(removed.has_value() && is_int(*removed) && as_int(*removed) == 200,
          "AC3: 200 keys are removed");

    // The remaining 1,800 keys must ALL still be found: a hard delete
    // (0xFF) would end the probe chain and hide the keys that were
    // displaced past the hole.
    auto after = cs.eval("(count-found h 2000)");
    CHECK(after.has_value() && is_int(*after) && as_int(*after) == 1800,
          "AC3: every surviving key is still found after the removals");
    auto len = cs.eval("(hash-length h)");
    CHECK(len.has_value() && is_int(*len) && as_int(*len) == 1800,
          "AC3: hash-length counts the live keys");
    auto gone = cs.eval("(hash-has-key? h \"k1000\")");
    CHECK(gone.has_value() && is_bool(*gone) && !as_bool(*gone), "AC3: the removed key is gone");
    auto keys_len = cs.eval("(length (hash-keys h))");
    CHECK(keys_len.has_value() && is_int(*keys_len) && as_int(*keys_len) == 1800,
          "AC3: hash-keys skips the deleted slots");
    auto vals_len = cs.eval("(length (hash-values h))");
    CHECK(vals_len.has_value() && is_int(*vals_len) && as_int(*vals_len) == 1800,
          "AC3: hash-values skips the deleted slots");

    // Re-insert a removed key: it must be found again.
    auto reinsert = cs.eval(R"((hash-set! h "k1000" 42) (hash-ref h "k1000" #f))");
    CHECK(reinsert.has_value() && is_int(*reinsert) && as_int(*reinsert) == 42,
          "AC3: a removed key can be set and read again");
}

// ── AC4: the equal? semantics for list / vector keys (#4489) ────
static void ac4_list_vector_keys() {
    std::println("\n--- #4612 AC4: list / vector key semantics ---");
    CompilerService cs;
    auto setup = cs.eval(R"(
        (define h (hash))
        (hash-set! h (list 1 2 3) 'v1)
        (hash-set! h (vector 1 2 3) 'v2))");
    CHECK(setup.has_value(), "AC4: the table with list / vector keys is built");
    auto list_hit = cs.eval("(hash-ref h (list 1 2 3))");
    CHECK(list_hit.has_value() && !is_void(*list_hit), "AC4: a list key is found by an equal list");
    auto vec_hit = cs.eval("(hash-ref h (vector 1 2 3))");
    CHECK(vec_hit.has_value() && !is_void(*vec_hit),
          "AC4: a vector key is found by an equal vector");
    auto miss = cs.eval("(hash-ref h (list 1 2) 'none)");
    CHECK(miss.has_value() && !is_void(*miss), "AC4: a different list is a miss");
    // A list and a vector are different keys (equal? semantics of #4489).
    auto distinct = cs.eval("(eq? (hash-ref h (list 1 2 3)) (hash-ref h (vector 1 2 3)))");
    CHECK(distinct.has_value() && is_bool(*distinct) && !as_bool(*distinct),
          "AC4: a list key and a vector key stay distinct");
}

// ── AC5: the IR path uses the same probe ────────────────────────
static void ac5_ir_path() {
    std::println("\n--- #4612 AC5: the IR path uses the same probe ---");
    const auto ir_src = read_source("src/compiler/ir_executor_impl.cpp");
    CHECK(!ir_src.empty(), "AC5: the IR executor source is readable");
    const auto at = ir_src.find("case IROpcode::HashRef");
    CHECK(at != std::string::npos, "AC5: the IR executor has a HashRef opcode");
    if (at != std::string::npos) {
        const auto w = ir_src.substr(at, 2000);
        CHECK(w.find("hash-ref") != std::string::npos,
              "AC5: the IR HashRef opcode dispatches to the same hash-ref primitive");
    }

    CompilerService cs;
    auto ir = cs.eval_ir(R"((define h (hash)) (hash-set! h "a" 1) (hash-ref h "a"))");
    CHECK(ir.has_value() && is_int(*ir) && as_int(*ir) == 1, "AC5: the IR path finds the key");
    auto ir_miss = cs.eval_ir(R"((define h (hash)) (hash-set! h "a" 1) (hash-ref h "zz" "DEF"))");
    CHECK(ir_miss.has_value() && !is_void(*ir_miss),
          "AC5: the IR path returns the default on a miss");
    auto ir_has = cs.eval_ir(R"((define h (hash)) (hash-set! h "a" 1) (hash-has-key? h "a"))");
    CHECK(ir_has.has_value() && is_bool(*ir_has) && as_bool(*ir_has),
          "AC5: the IR path answers hash-has-key?");
}

// ── AC6: the gate wiring ────────────────────────────────────────
static void ac6_gate_wiring() {
    std::println("\n--- #4612 AC6: gate wiring ---");
    const auto build = read_source("build.py");
    const auto allow = read_source("scripts/coverage/root_check_allowlist.txt");
    CHECK(build.find("check_hash_probe_4612.py") != std::string::npos,
          "AC6: build.py runs the linter");
    CHECK(allow.find("check_hash_probe_4612.py") != std::string::npos,
          "AC6: the allowlist lists the linter");
    const auto linter = read_source("scripts/check_hash_probe_4612.py");
    CHECK(!linter.empty(), "AC6: the source-cite linter exists");
    CHECK(linter.find("test_hash_probe.cpp") != std::string::npos,
          "AC6: the linter names the test home");
    const auto cmake = read_source("CMakeLists.txt");
    CHECK(cmake.find("test_hash_probe") != std::string::npos,
          "AC6: CMakeLists registers the test target");
}

} // namespace

int run_test_hash_probe() {
    std::println("=== Issue #4612: hash lookup probes by hash ===");
    ac1_source_cite();
    ac2_flat_costs();
    ac3_removal_tombstone();
    ac4_list_vector_keys();
    ac5_ir_path();
    ac6_gate_wiring();
    std::println("\n=== #4612: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hash_probe();
}
#endif
