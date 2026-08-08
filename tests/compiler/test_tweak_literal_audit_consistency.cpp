// @category: unit
// @reason: Issue #2799 — tweak-literal logs Committed before outer batch
// commit; batch/Guard abort must restore int AND mark status=RolledBack
// (same torn-audit class as #2793 replace-value).
//
//   AC1: lockless + public cite #2799; MutationSoAField::IntVal
//   AC2: Guard abort restores value + status=RolledBack
//   AC3: atomic-batch tweak then fail → value old + tweak log RolledBack
//   AC4: commit path keeps Committed
//   AC5: this suite + linter; no docs/design/2799-*; no test_issue_2799.cpp

#include "test_harness.hpp"

#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core;
import aura.core.ast;

namespace {

using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
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

static std::string merr_kind(CompilerService& cs, const EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!is_string(pairs[idx].car))
        return {};
    auto sidx = as_string_idx(pairs[idx].car);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

static NodeId find_literal_int(aura::ast::FlatAST& flat, std::int64_t want) {
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto v = flat.get(id);
        if (v.tag == NodeTag::LiteralInt && v.int_value == want)
            return id;
    }
    return NULL_NODE;
}

} // namespace

int run_test_tweak_literal_audit_consistency() {
    std::println("=== Issue #2799: tweak-literal audit status consistency ===");
    CHECK(true, "ac2799: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: source cites #2799 + IntVal ---");
        auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
        CHECK(!mut.empty() && !flat.empty(), "AC1: sources readable");
        auto ppos = mut.find("Issue #2799");
        if (ppos == std::string::npos)
            ppos = mut.find("mutate:tweak-literal");
        CHECK(ppos != std::string::npos, "AC1: public tweak-literal");
        auto pwin = mut.substr(ppos > 200 ? ppos - 200 : 0, 3500);
        CHECK(pwin.find("Issue #2799") != std::string::npos, "AC1: public cites #2799");
        CHECK(pwin.find("MutationSoAField::IntVal") != std::string::npos, "AC1: public IntVal");
        auto lpos = flat.find("eval_flat_apply_mutate_tweak_literal");
        CHECK(lpos != std::string::npos, "AC1: lockless helper");
        auto lwin = flat.substr(lpos, 2000);
        CHECK(lwin.find("Issue #2799") != std::string::npos, "AC1: lockless cites #2799");
        CHECK(lwin.find("MutationSoAField::IntVal") != std::string::npos, "AC1: lockless IntVal");
        CHECK(lwin.find("rollback_record_for_boundary_abort") != std::string::npos ||
                  lwin.find("RolledBack") != std::string::npos ||
                  lwin.find("2793") != std::string::npos,
              "AC1: documents RolledBack / #2793 path");
    }

    // ── AC2: Guard abort ──
    {
        std::println("\n--- AC2: Guard abort value + RolledBack ---");
        Evaluator ev;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto lit = flat.add_literal(static_cast<std::int64_t>(10));
        flat.root = lit;
        ev.set_workspace_flat(&flat);
        ev.set_workspace_pool(&pool);

        const auto old_val = flat.get(lit).int_value;
        const auto torn0 = flat.mutation_log_status_torn_total();
        ev.enter_mutation_boundary();
        const auto new_val = old_val + 5;
        (void)flat.add_mutation_with_rollback(
            lit, "tweak-literal", "Int", "Int", "ac2799", aura::ast::MutationStatus::Committed,
            static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal),
            static_cast<std::uint64_t>(old_val), static_cast<std::uint64_t>(new_val), true);
        flat.set_int(lit, new_val);
        CHECK(flat.get(lit).int_value == new_val, "AC2: mid-boundary value tweaked");
        CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::Committed,
              "AC2: mid-boundary Committed");
        ev.exit_mutation_boundary(false);
        CHECK(flat.get(lit).int_value == old_val, "AC2: value restored");
        CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::RolledBack,
              "AC2: status=RolledBack");
        CHECK(flat.mutation_log_status_torn_total() == torn0, "AC2: torn counter unchanged");
    }

    // ── AC3: atomic-batch tweak + fail ──
    {
        std::println("\n--- AC3: atomic-batch tweak then fail → RolledBack ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define f (lambda () 10))\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws, "AC3: workspace");
        auto lit = find_literal_int(*ws, 10);
        CHECK(lit != NULL_NODE, "AC3: found literal 10");
        const auto old_val = ws->get(lit).int_value;

        auto batch =
            cs.eval(std::format("(mutate:atomic-batch (list (list \"mutate:tweak-literal\" {} 7) "
                                "(list \"mutate:rebind\" \"no-such-2799\" \"(lambda () 0)\")))",
                                lit));
        CHECK(batch.has_value(), "AC3: batch returns");
        CHECK(!(is_bool(*batch) && as_bool(*batch)), "AC3: batch not success");
        CHECK(is_pair(*batch) && merr_kind(cs, *batch) == "batch-failed", "AC3: batch-failed");
        CHECK(ws->get(lit).int_value == old_val, "AC3: literal still old after batch fail");

        // Find last tweak-literal record; must be RolledBack.
        bool found_tweak = false;
        for (auto it = ws->mutation_log_view().rbegin(); it != ws->mutation_log_view().rend();
             ++it) {
            if (it->operator_name == "tweak-literal") {
                found_tweak = true;
                CHECK(it->status == aura::ast::MutationStatus::RolledBack,
                      "AC3: tweak-literal status=RolledBack after batch abort");
                break;
            }
        }
        CHECK(found_tweak, "AC3: tweak-literal log entry present");
    }

    // ── AC4: commit keeps Committed ──
    {
        std::println("\n--- AC4: success path keeps Committed ---");
        Evaluator ev;
        aura::ast::ASTArena arena;
        auto alloc = arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto lit = flat.add_literal(static_cast<std::int64_t>(3));
        flat.root = lit;
        ev.set_workspace_flat(&flat);
        ev.set_workspace_pool(&pool);
        ev.enter_mutation_boundary();
        (void)flat.add_mutation_with_rollback(
            lit, "tweak-literal", "Int", "Int", "ac2799-ok", aura::ast::MutationStatus::Committed,
            static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal), 3, 8, true);
        flat.set_int(lit, 8);
        ev.exit_mutation_boundary(true);
        CHECK(flat.get(lit).int_value == 8, "AC4: value stays 8");
        CHECK(flat.mutation_log_view().back().status == aura::ast::MutationStatus::Committed,
              "AC4: status stays Committed");
    }

    std::println("\n=== #2799 tweak-literal audit consistency: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_tweak_literal_audit_consistency();
}
#endif
