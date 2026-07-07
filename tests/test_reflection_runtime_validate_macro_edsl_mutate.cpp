// test_reflection_runtime_validate_macro_edsl_mutate.cpp — Issue #750:
// Runtime reflection schema validation for macro bodies + EDSL structs under
// Guard mutate (refines #734; non-duplicative with #454, #502, #654).
//
//   - AC1: query:reflection-schema-stats reachable (schema 750)
//   - AC2: reflect:validate-macro-body on macro workspace
//   - AC3: reflect:validate-edsl on SV Constraint/Class nodes
//   - AC4: Guard mutate bumps validated + hygiene-invariants-held stats
//   - AC5: multi-round mutate matrix monotonic
//   - AC6: query regression (reflect-postmutate, macro-hygiene-fiber-panic)

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace aura_issue_750_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t loc_hash(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (query:reflection-schema-stats) \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto validated = loc_hash(cs, "validated");
    const auto hygiene = loc_hash(cs, "hygiene-invariants-held");
    const auto violations = loc_hash(cs, "schema-violations");
    if (validated < 0 || hygiene < 0 || violations < 0)
        return -1;
    return validated + hygiene + violations;
}

struct EdslWorkspace {
    aura::ast::NodeId constraint_id = aura::ast::NULL_NODE;
    aura::ast::NodeId class_id = aura::ast::NULL_NODE;
};

static bool setup_macro_workspace(CompilerService& cs) {
    return cs.eval("(set-code \""
                   "(define-hygienic-macro (mk x) "
                   "  (list 'define (list 'v x) x)) "
                   "(define user-val 1) (mk 10)\")") &&
           cs.eval("(eval-current)").has_value();
}

static bool seed_edsl_workspace(CompilerService& cs, EdslWorkspace& ws) {
    if (!cs.eval("(set-code \"(define seed 1)\")"))
        return false;
    if (!cs.eval("(eval-current)").has_value())
        return false;
    auto* flat = cs.workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    if (!flat || !pool)
        return false;
    ws.constraint_id = flat->add_constraint(
        pool->intern("c_dist"),
        std::span<const aura::ast::SymId>{pool->intern("dist val inside {[0:255]};")});
    const std::vector<aura::ast::NodeId> cls_body{ws.constraint_id};
    ws.class_id = flat->add_class(pool->intern("Packet"), pool->intern("BasePkt"), cls_body);
    return ws.constraint_id != aura::ast::NULL_NODE && ws.class_id != aura::ast::NULL_NODE;
}

static aura::ast::NodeId first_macro_introduced_node(CompilerService& cs) {
    auto* flat = cs.workspace_flat();
    if (!flat)
        return aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
        if (flat->is_live_node(id) && flat->is_macro_introduced(id))
            return id;
    }
    return flat->root;
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:reflection-schema-stats (schema 750) ---");
    CHECK(setup_macro_workspace(cs), "macro workspace setup");
    auto h = cs.eval("(query:reflection-schema-stats)");
    CHECK(h && is_hash(*h), "reflection-schema-stats returns hash");
    CHECK(loc_hash(cs, "schema") == 750, "schema == 750");
    CHECK(loc_hash(cs, "validated") >= 0, "validated present");
    CHECK(loc_hash(cs, "hygiene-invariants-held") >= 0, "hygiene-invariants-held present");
    CHECK(loc_hash(cs, "schema-violations") >= 0, "schema-violations present");
    CHECK(loc_hash(cs, "stale-validation-prevented") >= 0, "stale-validation-prevented present");

    std::println("\n--- AC2: reflect:validate-macro-body ---");
    const auto validated0 = loc_hash(cs, "validated");
    const auto macro_nid = first_macro_introduced_node(cs);
    CHECK(macro_nid != aura::ast::NULL_NODE, "macro-introduced node found");
    auto macro_ok =
        cs.eval(std::format("(reflect:validate-macro-body {})", static_cast<int>(macro_nid)));
    CHECK(macro_ok && is_bool(*macro_ok) && as_bool(*macro_ok),
          "reflect:validate-macro-body returns #t on macro subtree");
    const auto validated1 = loc_hash(cs, "validated");
    std::println("  validated: {} -> {}", validated0, validated1);
    CHECK(validated1 > validated0, "validated grew after reflect:validate-macro-body");

    EdslWorkspace edsl{};
    CHECK(seed_edsl_workspace(cs, edsl), "EDSL workspace seeded");

    std::println("\n--- AC3: reflect:validate-edsl on Constraint/Class ---");
    const auto validated3a = loc_hash(cs, "validated");
    auto c_ok =
        cs.eval(std::format("(reflect:validate-edsl {})", static_cast<int>(edsl.constraint_id)));
    CHECK(c_ok && is_bool(*c_ok) && as_bool(*c_ok), "reflect:validate-edsl on Constraint #t");
    auto cls_ok =
        cs.eval(std::format("(reflect:validate-edsl {})", static_cast<int>(edsl.class_id)));
    CHECK(cls_ok && is_bool(*cls_ok) && as_bool(*cls_ok), "reflect:validate-edsl on Class #t");
    const auto validated3b = loc_hash(cs, "validated");
    std::println("  validated: {} -> {}", validated3a, validated3b);
    CHECK(validated3b >= validated3a + 2, "validated grew after EDSL validate calls");

    std::println("\n--- AC4: Guard mutate bumps reflection schema stats ---");
    const auto stats4a = stats_sum(cs);
    (void)cs.eval("(mutate:rebind \"user-val\" \"42\")");
    (void)cs.eval("(eval-current)");
    (void)cs.eval(std::format("(eda:update-constraint {} \"val < 64;\")",
                              static_cast<int>(edsl.constraint_id)));
    const auto stats4b = stats_sum(cs);
    std::println("  reflection schema sum: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "reflection schema stats monotonic after Guard mutate");

    std::println("\n--- AC5: multi-round mutate matrix ---");
    const auto stats5a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"user-val\" \"" + std::to_string(50 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval(
            std::format("(reflect:validate-edsl {})", static_cast<int>(edsl.constraint_id)));
    }
    const auto stats5b = stats_sum(cs);
    std::println("  reflection schema sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "stats monotonic over mutate matrix");

    std::println("\n--- AC6: query regression ---");
    auto rpm = cs.eval("(query:reflect-postmutate-stats)");
    auto mhf = cs.eval("(query:macro-hygiene-fiber-panic-stats)");
    auto reb = cs.eval("(query:reflect-edsl-bridge-stats)");
    CHECK(rpm && is_hash(*rpm), "reflect-postmutate-stats regression");
    CHECK(mhf && is_hash(*mhf), "macro-hygiene-fiber-panic-stats regression");
    CHECK(reb && is_int(*reb), "reflect-edsl-bridge-stats regression");
}

} // namespace aura_issue_750_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_issue_750_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}