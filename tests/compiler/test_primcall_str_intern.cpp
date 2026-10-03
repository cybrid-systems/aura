// @category: unit
// @reason: Issue #2577 — PrimCall string re-intern must content-intern
//          so hot loops with fixed string args do not grow heaps O(N×args).
//
//   AC1: N× (string-append x "!") — eval heap growth ≪ N
//   AC2: display correctness preserved (string-append still works)
//   AC3: distinct contents still distinct (two different appends)
//   AC4: source-cite + cmake + gate

#include "test_harness.hpp"
#include "compiler/hash_meta.h"
#include "compiler/runtime_shared.h"
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
extern "C" std::uint64_t aura_hash_splitmix64(std::int64_t key);
extern "C" std::uint64_t aura_hash_probe_slot(std::uint64_t h, std::uint64_t i, std::uint64_t cap);
extern "C" const FlatHashTable* aura_hash_get_flat_table(std::int64_t hash_val);
extern "C" void aura_test_reset_hash_key_eq_calls() noexcept;
extern "C" std::uint64_t aura_test_hash_key_eq_calls() noexcept;

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

// Same stop rules as OpHashRef: empty ends, tombstone skips, only 0x80
// calls aura_hash_key_eq. The IR emits this probe; the tally is the
// call count the acceptance compares to capacity.
static std::int64_t probe_like_ir(const FlatHashTable* ht, std::int64_t key) {
    const auto cap = ht->capacity;
    const auto h = aura_hash_splitmix64(key);
    const auto* meta = ht->metadata();
    const auto* keys = ht->keys();
    const auto* vals = ht->values();
    for (std::uint64_t i = 0; i < cap; ++i) {
        const auto slot = aura_hash_probe_slot(h, i, cap);
        const auto m = meta[slot];
        if (m == aura::compiler::hash::kEmptySlot)
            return 11;
        if (m == 0x7F)
            continue;
        if (m != 0x80)
            continue;
        if (aura_hash_key_eq(keys[slot], key) != 0)
            return vals[slot];
    }
    return 11;
}

static std::int64_t key_with_first_slot(std::uint64_t cap, bool slot0) {
    for (std::int64_t k = 1; k < 4096; ++k) {
        const auto slot = aura_hash_probe_slot(aura_hash_splitmix64(k), 0, cap);
        if ((slot == 0) == slot0)
            return k;
    }
    return 1;
}

// Issue #4319: OpHashRef open-addresses from splitmix64. An empty slot
// ends the probe. A tombstone does not. A hash-set key is found at its
// hashed slot, and eq runs once for a direct hit, not once per slot.
static void ac4319_ophashref_hashed_probe() {
    std::println("\n--- #4319: OpHashRef hashed probe ---");
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    const auto op = jit.find("case OpHashRef:");
    const auto set = jit.find("case OpHashSet:");
    CHECK(op != std::string::npos && set != std::string::npos && op < set, "4319: OpHashRef case");
    if (op != std::string::npos && set > op) {
        const auto win = jit.substr(op, set - op);
        CHECK(win.find("fn_hash_splitmix") != std::string::npos, "4319: splitmix call");
        CHECK(win.find("fn_hash_probe") != std::string::npos, "4319: probe call");
        CHECK(win.find("CreateCondBr(is_empty, miss_bb, tomb_bb)") != std::string::npos,
              "4319: empty ends at miss");
        CHECK(win.find("CreateCondBr(is_tomb, next_bb, occ_bb)") != std::string::npos,
              "4319: tombstone continues");
        CHECK(win.find("getInt8(0x80)") != std::string::npos, "4319: occupied is 0x80");
        const auto unlock = win.find("fn_unlock_workspace_read");
        const auto eq = win.find("fn_hash_key_eq");
        const auto relock =
            (eq == std::string::npos) ? std::string::npos : win.find("fn_lock_workspace_read", eq);
        CHECK(unlock != std::string::npos && eq != std::string::npos &&
                  relock != std::string::npos && unlock < eq && eq < relock,
              "4319: read lock dropped around key eq");
        CHECK(win.find("c64(11)") != std::string::npos, "4319: miss sentinel 11");
    }
    CHECK(jit.find("reg(\"aura_hash_splitmix64\"") != std::string::npos,
          "4319: splitmix registered");
    CHECK(jit.find("reg(\"aura_hash_probe_slot\"") != std::string::npos, "4319: probe registered");
    CHECK(read_file("tests/compiler/test_issue_4319.cpp").empty(), "4319: no test_issue file");
    CHECK(read_file("docs/design/4319-hash-ref-probe.md").empty(), "4319: no docs/design");
    CHECK(aura_hash_probe_slot(5, 0, 0) == 0, "4319: cap 0 probe is 0");

    constexpr std::uint64_t cap = 32;
    const auto hit0 = key_with_first_slot(cap, true);
    auto* full = FlatHashTable::create(cap);
    CHECK(full != nullptr, "4319: cap-32 table");
    if (full != nullptr) {
        for (std::uint64_t i = 0; i < cap; ++i) {
            full->metadata()[i] = 0x80;
            full->keys()[i] = 100000 + static_cast<std::int64_t>(i);
            full->values()[i] = 1;
        }
        const auto slot = aura_hash_probe_slot(aura_hash_splitmix64(hit0), 0, cap);
        CHECK(slot == 0, "4319: chosen key hashes to slot 0");
        full->keys()[slot] = hit0;
        full->values()[slot] = 4319;
        aura_test_reset_hash_key_eq_calls();
        CHECK(probe_like_ir(full, hit0) == 4319, "4319: slot-0 hit returns the value");
        const auto calls = aura_test_hash_key_eq_calls();
        CHECK(calls == 1, "4319: slot-0 hit calls eq once");
        CHECK(calls < cap, "4319: eq calls stay below capacity");
        FlatHashTable::destroy(full);
    }

    const auto buried = key_with_first_slot(cap, false);
    auto* stopped = FlatHashTable::create(cap);
    CHECK(stopped != nullptr, "4319: empty-stop table");
    if (stopped != nullptr) {
        const auto h = aura_hash_splitmix64(buried);
        const auto s0 = aura_hash_probe_slot(h, 0, cap);
        const auto s1 = aura_hash_probe_slot(h, 1, cap);
        CHECK(s0 != 0, "4319: buried key does not start at slot 0");
        CHECK(s0 != s1, "4319: probe step moves");
        stopped->metadata()[s1] = 0x80;
        stopped->keys()[s1] = buried;
        stopped->values()[s1] = 99;
        if (s0 != 0) {
            stopped->metadata()[0] = 0x80;
            stopped->keys()[0] = buried;
            stopped->values()[0] = 111;
        }
        aura_test_reset_hash_key_eq_calls();
        CHECK(probe_like_ir(stopped, buried) == 11, "4319: empty ends the probe");
        CHECK(aura_test_hash_key_eq_calls() == 0, "4319: empty does not call eq");
        FlatHashTable::destroy(stopped);
    }

    auto* tombs = FlatHashTable::create(cap);
    CHECK(tombs != nullptr, "4319: tombstone table");
    if (tombs != nullptr) {
        const auto h = aura_hash_splitmix64(buried);
        const auto s0 = aura_hash_probe_slot(h, 0, cap);
        const auto s1 = aura_hash_probe_slot(h, 1, cap);
        tombs->metadata()[s0] = 0x7F;
        tombs->metadata()[s1] = 0x80;
        tombs->keys()[s1] = buried;
        tombs->values()[s1] = 4242;
        if (s0 != 0 && s1 != 0) {
            tombs->metadata()[0] = 0x80;
            tombs->keys()[0] = buried;
            tombs->values()[0] = 111;
        }
        aura_test_reset_hash_key_eq_calls();
        CHECK(probe_like_ir(tombs, buried) == 4242, "4319: tombstone does not end the probe");
        CHECK(aura_test_hash_key_eq_calls() == 1, "4319: tombstone does not call eq");
        FlatHashTable::destroy(tombs);
    }

    const auto placed = key_with_first_slot(cap, false);
    const auto hash = aura_hash_alloc_tenant(4319);
    const auto pair = aura_alloc_pair_tenant(placed, 5150, 4319);
    CHECK(hash >= 0 && pair != 0, "4319: alloc hash and pair");
    CHECK(aura_hash_set_checked(hash, pair, 4319, 0) == 0, "4319: hash-set");
    CHECK(aura_hash_ref_checked(hash, placed, 4319, 0) == 5150, "4319: hash-set key is found");
    const auto* live = aura_hash_get_flat_table(hash);
    CHECK(live != nullptr && live->capacity >= cap, "4319: live table");
    if (live != nullptr) {
        const auto slot = aura_hash_probe_slot(aura_hash_splitmix64(placed), 0, live->capacity);
        CHECK(slot != 0, "4319: placed key is not slot 0");
        CHECK(live->metadata()[slot] == 0x80, "4319: hashed slot occupied");
        CHECK(live->keys()[slot] == placed, "4319: key stored at the hashed slot");
        aura_test_reset_hash_key_eq_calls();
        CHECK(probe_like_ir(live, placed) == 5150, "4319: probe finds the hash-set key");
        const auto calls = aura_test_hash_key_eq_calls();
        CHECK(calls == 1, "4319: direct hit calls eq once");
        CHECK(calls < live->capacity, "4319: eq calls are the probe length");
    }
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
    ac4319_ophashref_hashed_probe();
    std::println("\n=== #2577: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_primcall_str_intern();
}
#endif
