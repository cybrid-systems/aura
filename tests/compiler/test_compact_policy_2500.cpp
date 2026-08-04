// @category: unit
// @reason: Issue #2500 — Agent compact policy from mutation-concurrency
//          health + arena frag + GC defer + hold estimate.
//
//   AC1: Table-driven pure compute_compact_policy (fixture → mode)
//   AC2: Under MutationHold / defer / live pin → never Force
//   AC3: High frag + clear path → Force
//   AC4: Schema additive (query:compact-policy + aliases); no core gate change
//   AC5: Source-cite + CMake gate

#include "test_harness.hpp"

#include "compiler/compact_policy.hh"
#include "compiler/mutation_concurrency_health.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompactPolicyInput;
using aura::compiler::CompactPolicyMode;
using aura::compiler::CompilerService;
using aura::compiler::compute_compact_policy;
using aura::compiler::kCompactPolicyIssue;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
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

static std::int64_t href_int(CompilerService& cs, std::string_view query, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: table-driven pure policy ──
static void ac1_table() {
    std::println("\n--- #2500 AC1: table-driven pure policy ---");
    auto run = [](const char* name, CompactPolicyInput in, CompactPolicyMode want,
                  const char* want_reason) {
        auto r = compute_compact_policy(in);
        CHECK(r.mode == want, std::format("AC1[{}]: mode", name).c_str());
        CHECK(r.reason == want_reason, std::format("AC1[{}]: reason", name).c_str());
        if (in.should_defer_destructive_gc || in.active_guard_depth > 0 ||
            in.mutation_hold_active || in.pin_contract_fail_total > 0) {
            CHECK(r.mode != CompactPolicyMode::Force,
                  std::format("AC1[{}]: no Force under hold/pin", name).c_str());
        }
    };
    run("vacuous-skip", {}, CompactPolicyMode::Skip, "healthy-low-frag");
    {
        CompactPolicyInput in;
        in.recommend_split = true;
        in.frag_bp = 5000;
        run("split-batch", in, CompactPolicyMode::SplitBatch, "hold-estimate-split");
    }
    {
        CompactPolicyInput in;
        in.should_defer_destructive_gc = true;
        in.frag_bp = 5000;
        run("defer-soft", in, CompactPolicyMode::Soft, "defer-or-hold");
    }
    {
        CompactPolicyInput in;
        in.active_guard_depth = 1;
        in.frag_bp = 5000;
        run("guard-soft", in, CompactPolicyMode::Soft, "defer-or-hold");
    }
    {
        CompactPolicyInput in;
        in.mutation_hold_active = true;
        in.frag_bp = 5000;
        run("hold-soft", in, CompactPolicyMode::Soft, "defer-or-hold");
    }
    {
        CompactPolicyInput in;
        in.pin_contract_fail_total = 1;
        in.frag_bp = 5000;
        in.health_bp = 10000;
        run("pin-soft", in, CompactPolicyMode::Soft, "pin-or-densify");
    }
    {
        CompactPolicyInput in;
        in.densify_consistency_fail_total = 2;
        in.frag_bp = 5000;
        run("densify-soft", in, CompactPolicyMode::Soft, "pin-or-densify");
    }
    {
        CompactPolicyInput in;
        in.health_bp = 5000;
        in.health_budget_bp = 8000;
        in.frag_bp = 5000;
        run("low-health-soft", in, CompactPolicyMode::Soft, "low-health");
    }
    {
        CompactPolicyInput in;
        in.health_bp = 10000;
        in.health_budget_bp = 8000;
        in.frag_bp = 4500;
        run("high-frag-force", in, CompactPolicyMode::Force, "high-frag-clear");
    }
    {
        CompactPolicyInput in;
        in.health_bp = 10000;
        in.health_budget_bp = 8000;
        in.frag_bp = 2000;
        run("mid-frag-soft", in, CompactPolicyMode::Soft, "default-soft");
    }
    CHECK(kCompactPolicyIssue == 2500, "AC1: issue stamp");
}

// ── AC2: never Force under hold/pin (explicit) ──
static void ac2_never_force_under_hold() {
    std::println("\n--- #2500 AC2: never Force under hold/pin ---");
    CompactPolicyInput in;
    in.frag_bp = 9000;
    in.health_bp = 10000;
    in.should_defer_destructive_gc = true;
    auto r = compute_compact_policy(in);
    CHECK(r.mode == CompactPolicyMode::Soft, "AC2: defer → soft");
    CHECK(r.mode != CompactPolicyMode::Force, "AC2: not force");

    in = {};
    in.frag_bp = 9000;
    in.mutation_hold_active = true;
    r = compute_compact_policy(in);
    CHECK(r.mode == CompactPolicyMode::Soft, "AC2: mutation hold → soft");

    in = {};
    in.frag_bp = 9000;
    in.force_blocked_by_pin_total = 3;
    r = compute_compact_policy(in);
    CHECK(r.mode == CompactPolicyMode::Soft, "AC2: pin block → soft");
}

// ── AC3: high frag clear → Force ──
static void ac3_force_high_frag() {
    std::println("\n--- #2500 AC3: high frag + clear → Force ---");
    CompactPolicyInput in;
    in.health_bp = 10000;
    in.health_budget_bp = 8000;
    in.frag_bp = 4001;
    auto r = compute_compact_policy(in);
    CHECK(r.mode == CompactPolicyMode::Force, "AC3: Force");
    CHECK(r.reason == "high-frag-clear", "AC3: reason");
    CHECK(r.mode_name == "force", "AC3: mode name");
}

// ── AC4: query surface additive ──
static void ac4_query_surface() {
    std::println("\n--- #2500 AC4: query:compact-policy surface ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto h = cs.eval("(engine:metrics \"query:compact-policy\")");
    CHECK(h && is_hash(*h), "AC4: compact-policy is hash");
    CHECK(href_int(cs, "query:compact-policy", "schema-2500") == 2500, "AC4: schema-2500");
    CHECK(href_int(cs, "query:compact-policy", "issue-2500") == 2500, "AC4: issue-2500");
    CHECK(href_int(cs, "query:compact-policy", "compact-policy-wired") == 1, "AC4: wired");
    CHECK(href_int(cs, "query:compact-policy", "advisory") == 1, "AC4: advisory");
    CHECK(href_int(cs, "query:compact-policy", "mode-code") >= 0, "AC4: mode-code");
    // Aliases resolve (register_stats_impl).
    CHECK(href_int(cs, "orch:compact-policy", "schema-2500") == 2500, "AC4: orch alias");
    CHECK(href_int(cs, "arena:recommend-compact", "schema-2500") == 2500, "AC4: arena alias");
    // Subsystem queries still work (additive).
    CHECK(href_int(cs, "query:mutation-concurrency-health", "schema-2379") == 2379 ||
              href_int(cs, "query:mutation-concurrency-health",
                       "mutation-concurrency-health-wired") == 1,
          "AC4: health query still present");
}

// ── AC5: source + gate ──
static void ac5_source_gate() {
    std::println("\n--- #2500 AC5: source cite + CMake ---");
    auto hh = read_file("src/compiler/compact_policy.hh");
    CHECK(!hh.empty(), "AC5: compact_policy.hh");
    CHECK(hh.find("compute_compact_policy") != std::string::npos, "AC5: pure fn");
    CHECK(hh.find("2500") != std::string::npos, "AC5: #2500");
    auto src = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(src.find("query:compact-policy") != std::string::npos, "AC5: query registered");
    CHECK(src.find("orch:compact-policy") != std::string::npos, "AC5: orch alias");
    CHECK(src.find("arena:recommend-compact") != std::string::npos, "AC5: arena alias");
    auto cm = read_file("CMakeLists.txt");
    CHECK(cm.find("test_compact_policy_2500") != std::string::npos, "AC5: CMake");
}

} // namespace

int run_test_compact_policy_2500() {
    std::println("=== Issue #2500: Agent compact policy ===");
    ac1_table();
    ac2_never_force_under_hold();
    ac3_force_high_frag();
    ac4_query_surface();
    ac5_source_gate();
    std::println("\n=== #2500 summary: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_compact_policy_2500();
}
#endif
