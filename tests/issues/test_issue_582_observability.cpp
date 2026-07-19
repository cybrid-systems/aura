// @category: integration
// @reason: Issue #582 — eda-concurrency-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_582_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:eda-concurrency-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_sv_workspace(aura::compiler::CompilerService& cs, aura::ast::NodeId& property_id,
                               aura::ast::NodeId& coverpoint_id) {
    if (!cs.eval("(set-code \"(define base 10) (+ base 1)\")")) {
        return false;
    }
    if (!cs.eval("(eval-current)").has_value()) {
        return false;
    }
    const char* netlist = "interface:iface_a\n"
                          "property:prop_a:req ##1 ack\n"
                          "coverpoint:var_a:b0,b1\n";
    auto parsed = cs.eval(std::format("(eda:parse-netlist \"{}\")", netlist));
    if (!parsed || !aura::compiler::types::is_int(*parsed) ||
        aura::compiler::types::as_int(*parsed) < 3) {
        return false;
    }
    property_id = aura::ast::NULL_NODE;
    coverpoint_id = aura::ast::NULL_NODE;
    if (auto* ws = cs.workspace_flat()) {
        for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
            if (property_id == aura::ast::NULL_NODE &&
                ws->get(id).tag == aura::ast::NodeTag::Property)
                property_id = id;
            if (coverpoint_id == aura::ast::NULL_NODE &&
                ws->get(id).tag == aura::ast::NodeTag::Coverpoint)
                coverpoint_id = id;
        }
    }
    return property_id != aura::ast::NULL_NODE && coverpoint_id != aura::ast::NULL_NODE;
}

} // namespace aura_issue_582_detail

int main() {
    using namespace aura_issue_582_detail;

    std::println("=== Issue #582: eda-concurrency-stats hash ===");

    aura::compiler::CompilerService cs;
    aura::ast::NodeId property_id = aura::ast::NULL_NODE;
    aura::ast::NodeId coverpoint_id = aura::ast::NULL_NODE;
    CHECK(setup_sv_workspace(cs, property_id, coverpoint_id), "SV workspace setup + netlist parse");

    // AC1: query:eda-concurrency-stats returns hash
    {
        std::println("\n--- AC1: query:eda-concurrency-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:eda-concurrency-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:eda-concurrency-stats returns hash");
        CHECK(hash_int(cs, "sv-concurrent-mutate-deadlocks") >= 0,
              "sv-concurrent-mutate-deadlocks present");
        CHECK(hash_int(cs, "atomic-batch-sv-success") >= 0, "atomic-batch-sv-success present");
        CHECK(hash_int(cs, "feedback-during-steal-events") >= 0,
              "feedback-during-steal-events present");
        CHECK(hash_int(cs, "boundary-violation-on-sv") >= 0, "boundary-violation-on-sv present");
        CHECK(hash_int(cs, "eda-concurrency-schema") == 582, "eda-concurrency-schema == 582");
        CHECK(hash_int(cs, "eda-concurrency-total") >= 0, "eda-concurrency-total present");
        CHECK(hash_int(cs, "eda-concurrency-recommendation") >= 0,
              "eda-concurrency-recommendation present");
    }

    const auto batch_before = hash_int(cs, "atomic-batch-sv-success");
    const auto total_before = hash_int(cs, "eda-concurrency-total");

    // AC2: SV feedback + atomic batch under Guard
    {
        std::println("\n--- AC2: SV feedback + atomic batch ---");
        auto feedback =
            cs.eval(std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole\")",
                                static_cast<int>(coverpoint_id)));
        CHECK(feedback && aura::compiler::types::is_bool(*feedback) &&
                  aura::compiler::types::as_bool(*feedback),
              "eda:run-verification-feedback succeeds");
        auto batch = cs.eval(
            "(mutate:atomic-batch (list (list \"mutate:rebind\" \"base\" \"99\")) \"sv-batch\")");
        CHECK(batch && aura::compiler::types::is_bool(*batch) &&
                  aura::compiler::types::as_bool(*batch),
              "mutate:atomic-batch on SV workspace succeeds");
        const auto batch_after = hash_int(cs, "atomic-batch-sv-success");
        CHECK(batch_after > batch_before,
              std::format("atomic-batch-sv-success grew ({} -> {})", batch_before, batch_after));
    }

    // AC3: GC safepoint + second SV mutate cycle
    {
        std::println("\n--- AC3: GC safepoint + SV mutate cycle ---");
        (void)cs.eval("(mutate:request-gc-safepoint)");
        auto weaken = cs.eval(
            std::format("(eda:weaken-property {} \"reset\")", static_cast<int>(property_id)));
        CHECK(weaken && aura::compiler::types::is_bool(*weaken) &&
                  aura::compiler::types::as_bool(*weaken),
              "eda:weaken-property under Guard succeeds");
        const auto total_after = hash_int(cs, "eda-concurrency-total");
        CHECK(total_after >= total_before,
              std::format("eda-concurrency-total monotonic ({} -> {})", total_before, total_after));
        CHECK(hash_int(cs, "boundary-violation-on-sv") >= 0,
              "boundary-violation-on-sv readable after mutate cycle");
    }

    // AC4: related concurrency primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto multi = cs.eval("(engine:metrics \"query:multi-fiber-orchestration-stats\")");
        auto sv_batch = cs.eval("(engine:metrics \"query:atomic-batch-sv-stats-hash\")");
        auto feedback = cs.eval("(engine:metrics \"query:verification-feedback-loop-stats\")");
        auto sv_scale = cs.eval("(engine:metrics \"query:stable-ref-sv-scale-stats\")");
        CHECK(multi && aura::compiler::types::is_hash(*multi),
              "query:multi-fiber-orchestration-stats hash regression (#521)");
        CHECK(sv_batch && aura::compiler::types::is_hash(*sv_batch),
              "query:atomic-batch-sv-stats-hash regression (#632)");
        CHECK(feedback && aura::compiler::types::is_hash(*feedback),
              "query:verification-feedback-loop-stats hash regression (#579)");
        CHECK(sv_scale && aura::compiler::types::is_hash(*sv_scale),
              "query:stable-ref-sv-scale-stats hash regression (#581)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 147,
              "stats:count >= 147");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}