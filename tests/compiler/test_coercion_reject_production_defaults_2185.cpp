// @category: unit
// @reason: Issue #2185 — production default reject_apply_on_provenance_miss
// (refine #2102 + #2053). Soft apply under AURA_SANDBOX=off.
//
//   AC1: Production defaults + incomplete chain → no CoercionNode;
//        miss_reject_total++
//   AC2: Re-infer with active_mutation_id → complete stamp + apply
//   AC3: Dev defaults allow sentinel apply + force-audit
//   AC4: #2102 Soft/Strict unit path green; complete path unaffected
//   AC5: src-aligned tests under tests/compiler/

#include "test_harness.hpp"

#include "compiler/coercion_provenance_policy.hh"
#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/mutation_audit_wal.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.ast;
import aura.compiler.coercion_map;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::apply_coercion_map;
using aura::compiler::clear_coercion_active_mutation_context;
using aura::compiler::CoercionMap;
using aura::compiler::CompilerService;
using aura::compiler::consume_provenance_miss_for_boundary;
using aura::compiler::force_audit_on_provenance_miss;
using aura::compiler::g_coercion_provenance_complete_total;
using aura::compiler::g_coercion_provenance_miss_reject_total;
using aura::compiler::g_coercion_provenance_miss_total;
using aura::compiler::g_coercion_provenance_sentinel_total;
using aura::compiler::kCoercionProvenanceRejectProductionIssue;
using aura::compiler::reject_apply_on_provenance_miss;
using aura::compiler::reset_coercion_provenance_miss_policy_for_test;
using aura::compiler::set_coercion_active_mutation_context;
using aura::compiler::set_reject_apply_on_provenance_miss;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::audit_wal::reset_audit_wal_for_test;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
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

static void clear_env(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}

static void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static void reset_process() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    reset_audit_wal_for_test();
    reset_for_test();
    reset_coercion_provenance_miss_policy_for_test();
    clear_coercion_active_mutation_context();
    (void)consume_provenance_miss_for_boundary();
    set_mode(SandboxMode::Off);
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
    clear_env("AURA_TYPED_AUDIT");
    clear_env("AURA_MUTATION_AUDIT_WAL");
    clear_env("AURA_PERSIST_DIR");
    clear_env("AURA_LINEAR_ENFORCE");
    clear_env("AURA_COERCION_PROVENANCE_REJECT");
    clear_env("AURA_HARD_FIBER_ISOLATION");
    clear_env("AURA_GRANT_EPOCH_RETAIN");
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void make_coerce_tree(FlatAST& flat, StringPool& pool, aura::ast::NodeId& parent,
                             aura::ast::NodeId& lit) {
    auto x = pool.intern("x");
    auto xv = flat.add_variable(x);
    lit = flat.add_literal(7);
    parent = flat.add_call(xv, std::array<aura::ast::NodeId, 1>{lit});
    flat.root = parent;
}

} // namespace

int run_test_coercion_reject_production_defaults_2185() {
    std::println("=== Issue #2185: production reject_apply_on_provenance_miss ===");
    CHECK(kCoercionProvenanceRejectProductionIssue == 2185, "issue stamp");

    // ── AC1: production defaults → reject incomplete insert ──
    {
        std::println("\n--- AC1: production defaults reject incomplete ---");
        reset_process();
        apply_production_security_defaults();
        CHECK(force_audit_on_provenance_miss(), "AC1: force-audit stays true");
        CHECK(reject_apply_on_provenance_miss(), "AC1: reject on under production");

        FlatAST flat;
        StringPool pool;
        aura::ast::NodeId parent = 0, lit = 0;
        make_coerce_tree(flat, pool, parent, lit);
        CoercionMap map;
        map.add(parent, 1, lit, 1, 99, 0, 0, 0, 0);

        const auto reject0 = g_coercion_provenance_miss_reject_total.load();
        const auto miss0 = g_coercion_provenance_miss_total.load();
        const auto size0 = flat.size();
        const auto n = apply_coercion_map(flat, map);
        CHECK(n == 0, "AC1: no CoercionNode insert");
        CHECK(flat.size() == size0, "AC1: AST size unchanged");
        CHECK(g_coercion_provenance_miss_total.load() > miss0, "AC1: miss counted");
        CHECK(g_coercion_provenance_miss_reject_total.load() > reject0, "AC1: reject total++");
    }

    // ── AC2: re-infer with active mutation context → complete apply ──
    {
        std::println("\n--- AC2: re-infer with active_mutation_id ---");
        reset_process();
        apply_production_security_defaults();
        CHECK(reject_apply_on_provenance_miss(), "reject still on");

        FlatAST flat;
        StringPool pool;
        aura::ast::NodeId parent = 0, lit = 0;
        make_coerce_tree(flat, pool, parent, lit);

        // First apply rejects.
        CoercionMap map;
        map.add(parent, 1, lit, 1, 99, 0, 0, 0, 0);
        CHECK(apply_coercion_map(flat, map) == 0, "first apply rejected");

        // Re-infer: stamp active mutation id (Agent closed-loop) + map entry.
        set_coercion_active_mutation_context(/*mutation_id=*/9001, /*predicate=*/0);
        aura::ast::MutationRecord rec{};
        rec.mutation_id = 9001;
        rec.target_node = lit;
        rec.parent_id = parent;
        rec.operator_name = "test-2185-reinfer";
        rec.status = aura::ast::MutationStatus::Committed;
        flat.all_mutations().push_back(rec);

        CoercionMap map2;
        map2.add(parent, 1, lit, 1, 99, 0, 0, static_cast<std::uint32_t>(lit), 9001);
        const auto complete0 = g_coercion_provenance_complete_total.load();
        const auto n2 = apply_coercion_map(flat, map2);
        CHECK(n2 > 0, "AC2: insert after complete provenance");
        CHECK(g_coercion_provenance_complete_total.load() > complete0, "AC2: complete++");
        clear_coercion_active_mutation_context();
    }

    // ── AC3: AURA_SANDBOX=off → soft apply + force-audit ──
    {
        std::println("\n--- AC3: sandbox=off soft apply ---");
        reset_process();
        set_env("AURA_SANDBOX", "off");
        apply_production_security_defaults();
        CHECK(force_audit_on_provenance_miss(), "AC3: force-audit on");
        CHECK(!reject_apply_on_provenance_miss(), "AC3: reject off under sandbox=off");

        FlatAST flat;
        StringPool pool;
        aura::ast::NodeId parent = 0, lit = 0;
        make_coerce_tree(flat, pool, parent, lit);
        CoercionMap map;
        map.add(parent, 1, lit, 1, 99, 0, 0, 0, 0);
        const auto sent0 = g_coercion_provenance_sentinel_total.load();
        const auto n = apply_coercion_map(flat, map);
        CHECK(n > 0, "AC3: soft apply inserts CoercionNode");
        CHECK(g_coercion_provenance_sentinel_total.load() > sent0, "AC3: sentinel stamped");
        clear_env("AURA_SANDBOX");
    }

    // ── AC3b: env override canary ──
    {
        std::println("\n--- AC3b: AURA_COERCION_PROVENANCE_REJECT override ---");
        reset_process();
        apply_production_security_defaults();
        CHECK(reject_apply_on_provenance_miss(), "prod reject");
        set_env("AURA_COERCION_PROVENANCE_REJECT", "soft");
        apply_production_security_defaults();
        CHECK(!reject_apply_on_provenance_miss(), "env soft overrides prod");
        clear_env("AURA_COERCION_PROVENANCE_REJECT");
        set_env("AURA_SANDBOX", "off");
        set_env("AURA_COERCION_PROVENANCE_REJECT", "reject");
        apply_production_security_defaults();
        CHECK(reject_apply_on_provenance_miss(), "env reject under sandbox=off");
        clear_env("AURA_SANDBOX");
        clear_env("AURA_COERCION_PROVENANCE_REJECT");
    }

    // ── AC4: complete path unaffected under production reject ──
    {
        std::println("\n--- AC4: complete provenance still applies ---");
        reset_process();
        apply_production_security_defaults();
        FlatAST flat;
        StringPool pool;
        aura::ast::NodeId parent = 0, lit = 0;
        make_coerce_tree(flat, pool, parent, lit);
        aura::ast::MutationRecord rec{};
        rec.mutation_id = 4242;
        rec.target_node = lit;
        rec.parent_id = parent;
        rec.operator_name = "happy-2185";
        rec.status = aura::ast::MutationStatus::Committed;
        flat.all_mutations().push_back(rec);
        CoercionMap map;
        map.add(parent, 1, lit, 1, 99, 0, 0, static_cast<std::uint32_t>(lit), 4242);
        const auto reject0 = g_coercion_provenance_miss_reject_total.load();
        const auto n = apply_coercion_map(flat, map);
        CHECK(n > 0, "AC4: complete path inserts");
        CHECK(g_coercion_provenance_miss_reject_total.load() == reject0,
              "AC4: reject total stable on complete");
    }

    // ── AC5: query + source ──
    {
        std::println("\n--- AC5: query schema-2185 + source wiring ---");
        reset_process();
        apply_production_security_defaults();
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "schema-2185") == 2185, "schema-2185");
        CHECK(href(cs, "issue-2185") == 2185, "issue-2185");
        CHECK(href(cs, "coercion-provenance-reject-production-wired") == 1, "wired");
        CHECK(href(cs, "reject-apply-on-provenance-miss") == 1, "query reject on");
        CHECK(href(cs, "force-audit-on-provenance-miss") == 1, "query force-audit");
        CHECK(href(cs, "schema-2102") == 2102, "2102 lineage retained");
        CHECK(href(cs, "coercion-provenance-miss-reject-total") >= 0, "reject total key");

        const auto sd = read_file("src/compiler/security_defaults.hh");
        const auto pol = read_file("src/compiler/coercion_provenance_policy.hh");
        const auto cm = read_file("src/compiler/coercion_map.ixx");
        CHECK(sd.find("2185") != std::string::npos, "security_defaults cites 2185");
        CHECK(sd.find("apply_production_coercion_provenance_defaults") != std::string::npos,
              "defaults call production coercion");
        CHECK(pol.find("set_reject_apply_on_provenance_miss") != std::string::npos,
              "policy header");
        CHECK(cm.find("2185") != std::string::npos, "coercion_map cites 2185");
    }

    reset_process();
    apply_dev_audit_defaults();

    std::println("\n=== #2185 production coercion reject defaults: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_coercion_reject_production_defaults_2185();
}
#endif
