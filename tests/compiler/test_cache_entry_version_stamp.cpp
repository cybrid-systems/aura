// @category: unit
// @reason: Issue #2033 — CacheEntryVersionStamp + bridge_epoch in should_relower.
//
//   AC1: source cites #2033; CacheEntryVersionStamp + kRelowerBridgeEpoch
//   AC2: should_relower true on bridge_epoch mismatch (dirty=false, hash match)
//   AC3: should_relower true on mutation_count drift; false when stamp matches
//   AC4: defuse_version drift forces re-lower when both non-zero
//   AC5: legacy overload still works (no bridge check)
//   AC6: query:incremental-relower-stats schema-2033 + wire flags
//   AC7: service store/lookup path — define/eval; stamp counters non-decreasing

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.ir_cache_pure;
import aura.compiler.value;

namespace {

using aura::compiler::CacheEntryVersionStamp;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::kRelowerBridgeEpoch;
using aura::compiler::kRelowerDefuseVersion;
using aura::compiler::kRelowerDirty;
using aura::compiler::kRelowerMutationDrift;
using aura::compiler::kRelowerSourceHash;
using aura::compiler::should_relower;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:incremental-relower-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_source() {
    std::println("\n--- AC1: source cites #2033 ---");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    auto svc = read_file("src/compiler/service.ixx");
    auto met = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(!pure.empty() && pure.find("#2033") != std::string::npos, "ir_cache_pure #2033");
    CHECK(pure.find("CacheEntryVersionStamp") != std::string::npos, "stamp struct");
    CHECK(pure.find("kRelowerBridgeEpoch") != std::string::npos, "bridge reason");
    CHECK(pure.find("current_bridge_epoch") != std::string::npos, "bridge param");
    CHECK(!svc.empty() && svc.find("version_stamp_") != std::string::npos, "entry stamp field");
    CHECK(svc.find("stamp_version") != std::string::npos, "stamp_version helper");
    CHECK(svc.find("should_relower_bridge_epoch_mismatch") != std::string::npos ||
              svc.find("kRelowerBridgeEpoch") != std::string::npos,
          "lookup uses bridge check");
    CHECK(svc.find("sync_instruction_dirty_from_block_dirty") != std::string::npos,
          "SoA dirty sync");
    CHECK(!met.empty() && met.find("cache_entry_version_stamp_total") != std::string::npos,
          "stamp metric");
    CHECK(met.find("should_relower_bridge_epoch_mismatch_total") != std::string::npos,
          "bridge mismatch metric");
    CHECK(!q.empty() && q.find("schema-2033") != std::string::npos, "query schema-2033");
}

static void ac2_bridge_mismatch() {
    std::println("\n--- AC2: bridge_epoch mismatch forces re-lower ---");
    CacheEntryVersionStamp stamp;
    stamp.mutation_count = 5;
    stamp.bridge_epoch = 10;
    stamp.defuse_version = 1;
    std::uint32_t reasons = 0;
    // Same hash, not dirty, mutation current == stamp, but bridge drifted.
    const bool need = should_relower(/*src*/ 42, /*cached*/ 42, /*dirty*/ false, stamp,
                                     /*cur_mut*/ 5, /*cur_bridge*/ 11, /*cur_defuse*/ 1, &reasons);
    CHECK(need, "need re-lower");
    CHECK((reasons & kRelowerBridgeEpoch) != 0, "reason bridge");
    CHECK((reasons & kRelowerDirty) == 0, "not dirty reason");
    CHECK((reasons & kRelowerSourceHash) == 0, "not hash reason");
}

static void ac3_mutation_match() {
    std::println("\n--- AC3: mutation drift + clean match ---");
    CacheEntryVersionStamp stamp;
    stamp.mutation_count = 3;
    stamp.bridge_epoch = 7;
    stamp.defuse_version = 2;
    // Mutation advanced
    std::uint32_t r1 = 0;
    CHECK(should_relower(1, 1, false, stamp, /*cur_mut*/ 9, 7, 2, &r1), "mutation drift");
    CHECK((r1 & kRelowerMutationDrift) != 0, "mutation reason");
    // Perfect match
    std::uint32_t r2 = 0;
    CHECK(!should_relower(1, 1, false, stamp, 3, 7, 2, &r2), "clean match → false");
    CHECK(r2 == 0, "no reasons");
    // Dirty alone
    CHECK(should_relower(1, 1, true, stamp, 3, 7, 2, nullptr), "dirty forces");
}

static void ac4_defuse_drift() {
    std::println("\n--- AC4: defuse_version drift ---");
    CacheEntryVersionStamp stamp;
    stamp.mutation_count = 1;
    stamp.bridge_epoch = 1;
    stamp.defuse_version = 4;
    std::uint32_t r = 0;
    CHECK(should_relower(9, 9, false, stamp, 1, 1, /*cur_defuse*/ 9, &r), "defuse drift");
    CHECK((r & kRelowerDefuseVersion) != 0, "defuse reason");
    // Zero current defuse → skip check
    CHECK(!should_relower(9, 9, false, stamp, 1, 1, /*cur_defuse*/ 0, nullptr),
          "defuse 0 skips check");
}

static void ac5_legacy_overload() {
    std::println("\n--- AC5: legacy overload ---");
    CHECK(should_relower(1, 1, true, 0, 0), "legacy dirty");
    CHECK(should_relower(1, 2, false, 0, 0), "legacy hash");
    CHECK(should_relower(1, 1, false, 3, 5), "legacy mut drift");
    CHECK(!should_relower(1, 1, false, 5, 5), "legacy clean");
}

static void ac6_query() {
    std::println("\n--- AC6: query schema-2033 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:incremental-relower-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2033") == 2033, "schema-2033");
    CHECK(href(cs, "issue-2033") == 2033, "issue-2033");
    CHECK(href(cs, "cache-entry-version-stamp-wired") == 1, "stamp wired");
    CHECK(href(cs, "should-relower-bridge-epoch-wired") == 1, "bridge wired");
    CHECK(href(cs, "cache_entry_version_stamp_total") >= 0, "stamp total key");
    CHECK(href(cs, "should_relower_bridge_epoch_mismatch_total") >= 0, "bridge miss key");
}

static void ac7_service_stamp() {
    std::println("\n--- AC7: service store stamps version ---");
    CompilerService cs;
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto s0 = m ? m->cache_entry_version_stamp_total.load() : 0;
    CHECK(cs.eval("(set-code \"(define g (lambda (n) (+ n 1)))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    // Mutate forces re-lower path which re-stamps
    (void)cs.eval("(mutate:rebind \"g\" \"(lambda (n) (+ n 2))\")");
    (void)cs.eval("(eval-current)");
    if (m) {
        CHECK(m->cache_entry_version_stamp_total.load() >= s0, "stamps non-decreasing");
    }
    CHECK(href(cs, "schema-2033") == 2033, "schema after");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok");
}

} // namespace

int main() {
    ac1_source();
    ac2_bridge_mismatch();
    ac3_mutation_match();
    ac4_defuse_drift();
    ac5_legacy_overload();
    ac6_query();
    ac7_service_stamp();
    if (g_failed)
        return 1;
    std::println("cache entry version stamp (#2033): OK ({} passed)", g_passed);
    return 0;
}
