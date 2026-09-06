// test_production_sweep.cpp — fiber production sweep (standalone; SIGSEGV in batch)

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"
#include "core/workspace_epoch.hh"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

// Issue #1202/#1203/#1215/#1228 (#1978 renamed): issue# moved from filename to header.
// test_production_sweep_1202_1228.cpp — Issues #1202–#1228 Phase 1


using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Issue #3590: bootstrap order matrix (start-then-arm / unarmed / env override).
static void ac3590_bootstrap_order_matrix() {
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::typed_audit::production_defaults_active;
    using aura::core::query_epoch_strict;
    using aura::core::set_query_epoch_strict;

    std::println("\n--- #3590: bootstrap order matrix (start-then-arm / unarmed) ---");

    // Unarmed: observe-only is the contract.
    apply_dev_audit_defaults();
    ::setenv("AURA_SANDBOX", "off", 1);
    ::unsetenv("AURA_QUERY_EPOCH_STRICT");
    CHECK(production_defaults_active() == 0, "3590: unarmed probe");
    CHECK(!query_epoch_strict(), "unbootstrapped process must not report strict armed");

    // Start then arm: CompilerService already constructed above in main;
    // arming after start must flip QueryEpoch strict on.
    apply_production_audit_defaults();
    CHECK(query_epoch_strict(), "epoch strict must be armed under production defaults (#3075)");

    // Restore Soft, then arm-before-start: apply production then a fresh service.
    apply_dev_audit_defaults();
    ::setenv("AURA_SANDBOX", "off", 1);
    apply_production_audit_defaults();
    {
        CompilerService armed;
        CHECK(armed.eval("(+ 1 1)").has_value(), "3590: arm-then-start eval");
        CHECK(query_epoch_strict(), "3590: arm-then-start stays strict");
    }

    // AC2: explicit AURA_QUERY_EPOCH_STRICT override wins over unarmed default.
    apply_dev_audit_defaults();
    ::setenv("AURA_SANDBOX", "off", 1);
    ::setenv("AURA_QUERY_EPOCH_STRICT", "1", 1);
    set_query_epoch_strict(true);
    CHECK(production_defaults_active() == 0, "3590 AC2: still unarmed");
    CHECK(query_epoch_strict(), "3590 AC2: AURA_QUERY_EPOCH_STRICT override arms strict");
    ::unsetenv("AURA_QUERY_EPOCH_STRICT");
    set_query_epoch_strict(false);
    apply_dev_audit_defaults();
    ::setenv("AURA_SANDBOX", "off", 1);
}

} // namespace

int main() {
    CompilerService cs;

    {
        auto r = cs.eval("(engine:metrics \"query:production-sweep-1202-1228-stats\")");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "schema") == 1202, "schema");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "parallel-orch-scaffold") == 1,
              "parallel orch");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "self-healing-hooks-active") == 1,
              "self-heal");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "pure-analysis-pass-asserts") == 1,
              "pure analysis");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "agent-fiber-safepoint-wired") ==
                  1,
              "agent safepoint");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "dirty-propagation-module") == 1,
              "dirty prop");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "multi-fiber-mailbox-typed") == 1,
              "mf mailbox");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "hot-path-primitives-module") == 1,
              "hot path prims");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "eda-parse-common-dedup") == 1,
              "eda parse");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "issue-1228") == 1228,
              "issue-1228");
    }

    // #1215 production health composite
    {
        auto r = cs.eval("(stats:get \"query:production-health\")");
        CHECK(r && is_hash(*r), "production-health is hash");
        CHECK(href(cs, "query:production-health", "schema") == 1215, "health schema");
        CHECK(href(cs, "query:production-health", "score") == 100, "fresh score 100");
        CHECK(href(cs, "query:production-health", "healthy") == 1, "healthy");
    }

    // #1203 SelfHealingHook fires on quota violation
    {
        // Set a tiny memory quota and violate it
        auto set = cs.eval("(resource:quota-set \"memory\" 1)");
        (void)set;
        auto chk = cs.eval("(resource:quota-check \"memory\" 100)");
        CHECK(chk && is_bool(*chk) && !as_bool(*chk), "quota-check rejects over-limit");
    }

    {
        auto a = cs.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    // Issue #3590: bootstrap-order matrix for QueryEpoch strict (#3075 x #3586).
    ac3590_bootstrap_order_matrix();

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1202–#1228: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
