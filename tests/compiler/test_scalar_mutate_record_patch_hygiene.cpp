// @category: unit
// @reason: Issue #3131 — Close MacroIntroduced default-deny on scalar/metadata
// mutate prims (replace-value / replace-type / record-patch); structural
// covered, scalar residual.
//
//   AC1: source cites #3131 in evaluator_primitives_mutate.cpp —
//        record-patch body calls reject_structural_macro_hygiene +
//        propagate_macro_introduced_marker on MacroIntroduced node.
//   AC2: default mutate:record-patch on MacroIntroduced LiteralInt fails
//        closed with reason "hygiene" (no :allow-macro?).
//   AC3: :allow-macro? #t opts out — record-patch succeeds (mid >= 0)
//        + propagate_macro_introduced_marker fires for parity with
//        replace-type / replace-value.
//   AC4: global (hygiene:set-allow-macro-mutate! #t) opts out.
//   AC5: deny path bumps existing reject counters (no new metric keys).
//   AC6: chokepoint — guard is the only entry, after node resolve and
//        before flat.add_mutation; lockless / atomic-batch sub-ops that
//        route through record-patch inherit the same guard via this site.

#include "test_harness.hpp"

#include <cstdint>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerService;
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

} // namespace

int run_test_scalar_mutate_record_patch_hygiene() {
    std::println("=== Issue #3131: scalar/metadata mutate MacroIntroduced hygiene ===");
    CHECK(true, "ac3131: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: record-patch body cites #3131 + guard ---");
        auto src = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(!src.empty(), "AC1: mutate primitives readable");
        auto pos = src.find("add_mutate(\"mutate:record-patch\"");
        CHECK(pos != std::string::npos, "AC1: record-patch present");
        auto end = src.find("add_mutate(\"mutate:query-and-replace\"", pos);
        if (end == std::string::npos)
            end = pos + 6000;
        auto win = src.substr(pos, end - pos);
        CHECK(win.find("Issue #3131") != std::string::npos, "AC1: cites #3131");
        CHECK(win.find("reject_structural_macro_hygiene") != std::string::npos,
              "AC1: reject_structural_macro_hygiene called");
        CHECK(win.find("propagate_macro_introduced_marker") != std::string::npos,
              "AC1: propagate_macro_introduced_marker called");
        CHECK(win.find("parse_allow_macro_opt_out") != std::string::npos, "AC1: opt-out parse");
        CHECK(win.find("was_macro_rp") != std::string::npos, "AC1: was_macro captured");
        CHECK(win.find("parse_no_auto_restamp_opt_out") != std::string::npos,
              "AC1: no-auto-restamp opt-out wired");
    }

    // ── AC2: default fail closed ──
    {
        std::println("\n--- AC2: default mutate:record-patch on MacroIntroduced fails closed ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define target-rp 42)\")").has_value(), "AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval");
        auto find_r = cs.eval("(car (query :find \"target-rp\"))");
        CHECK(find_r && is_int(*find_r), "AC2: find target-rp");
        auto nid = static_cast<aura::ast::NodeId>(as_int(*find_r));
        CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", nid)).has_value(),
              "AC2: stamp MacroIntroduced");
        auto r = cs.eval(std::format("(mutate:record-patch {} \"op-name\" \"summary\")", nid));
        CHECK(r.has_value(), "AC2: record-patch returns");
        CHECK(is_pair(*r) && merr_kind(cs, *r) == "hygiene",
              "AC2: hygiene reason on MacroIntroduced target");
    }

    // ── AC3: :allow-macro? #t opts out ──
    {
        std::println("\n--- AC3: :allow-macro? #t opts out of record-patch hygiene ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define target-rp2 7)\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        auto find_r = cs.eval("(car (query :find \"target-rp2\"))");
        CHECK(find_r && is_int(*find_r), "AC3: find target-rp2");
        auto nid = static_cast<aura::ast::NodeId>(as_int(*find_r));
        CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", nid)).has_value(),
              "AC3: stamp MacroIntroduced");
        auto denied = cs.eval(std::format("(mutate:record-patch {} \"op-name\" \"summary\")", nid));
        CHECK(denied.has_value() && merr_kind(cs, *denied) == "hygiene",
              "AC3: denied without allow");
        auto allowed = cs.eval(
            std::format("(mutate:record-patch {} \"op-name\" \"summary\" :allow-macro? #t)", nid));
        CHECK(allowed.has_value(), "AC3: :allow-macro? #t returns");
        CHECK(is_int(*allowed), "AC3: success mid is int");
        CHECK(as_int(*allowed) >= 0, "AC3: mid >= 0 (audit log entry created)");
    }

    // ── AC4: global allow_macro_mutate_=#t opts out ──
    {
        std::println("\n--- AC4: global (hygiene:set-allow-macro-mutate! #t) opts out ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define target-rp3 9)\")").has_value(), "AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
        auto find_r = cs.eval("(car (query :find \"target-rp3\"))");
        CHECK(find_r && is_int(*find_r), "AC4: find target-rp3");
        auto nid = static_cast<aura::ast::NodeId>(as_int(*find_r));
        CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", nid)).has_value(),
              "AC4: stamp MacroIntroduced");
        // First confirm default deny (baseline).
        auto denied = cs.eval(std::format("(mutate:record-patch {} \"op-name\" \"summary\")", nid));
        CHECK(denied.has_value() && merr_kind(cs, *denied) == "hygiene",
              "AC4: denied before global flag");
        CHECK(cs.eval("(hygiene:set-allow-macro-mutate! #t)").has_value(),
              "AC4: set global allow_macro_mutate_");
        auto allowed =
            cs.eval(std::format("(mutate:record-patch {} \"op-name\" \"summary\")", nid));
        CHECK(allowed.has_value(), "AC4: global allow returns");
        CHECK(is_int(*allowed), "AC4: success mid is int");
        CHECK(as_int(*allowed) >= 0, "AC4: mid >= 0");
        // Reset global for clean state on subsequent tests.
        CHECK(cs.eval("(hygiene:set-allow-macro-mutate! #f)").has_value(), "AC4: reset global");
    }

    // ── AC5: counters bump on deny ──
    // record-patch body now calls reject_structural_macro_hygiene which
    // bumps naked_macro_mutate_attempt + macro_hygiene_provenance_hits_total
    // + sets last_hygiene_blame_node. Same metric surface as
    // replace-type / replace-value — no new keys.
    {
        std::println("\n--- AC5: counters bump on MacroIntroduced deny ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define target-rp4 11)\")").has_value(), "AC5: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC5: eval");
        auto find_r = cs.eval("(car (query :find \"target-rp4\"))");
        CHECK(find_r && is_int(*find_r), "AC5: find target-rp4");
        auto nid = static_cast<aura::ast::NodeId>(as_int(*find_r));
        CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", nid)).has_value(),
              "AC5: stamp MacroIntroduced");
        auto denied = cs.eval(std::format("(mutate:record-patch {} \"op-name\" \"summary\")", nid));
        CHECK(denied.has_value() && merr_kind(cs, *denied) == "hygiene",
              "AC5: deny fires + reason hygiene");
        // The hash key for last_hygiene_blame_node — kebab-case mapping.
        // Confirm blame points to the rejected nid (existing key, no new keys).
        auto blame_node = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:macro-hygiene-provenance-stats\") "
                        "\"last-hygiene-blame-node\")"));
        CHECK(blame_node && is_int(*blame_node), "AC5: last-hygiene-blame-node readable");
        CHECK(as_int(*blame_node) == static_cast<std::int64_t>(nid),
              "AC5: blame node id matches rejected nid");
    }

    // ── AC6: chokepoint verification ──
    // Guard is the only entry: positioned AFTER node resolution (so we
    // know the target id is valid) and BEFORE flat.add_mutation (so
    // the audit log entry can't be created for a MacroIntroduced node
    // under production defaults). Lockless / atomic-batch sub-ops that
    // dispatch to record-patch inherit the same guard via this site.
    {
        std::println("\n--- AC6: chokepoint (guard between resolve and add_mutation) ---");
        auto src = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        auto pos = src.find("add_mutate(\"mutate:record-patch\"");
        auto end = src.find("add_mutate(\"mutate:query-and-replace\"", pos);
        if (end == std::string::npos)
            end = pos + 6000;
        auto win = src.substr(pos, end - pos);
        auto node_resolve = win.find("auto node = static_cast<aura::ast::NodeId>");
        auto guard = win.find("reject_structural_macro_hygiene");
        auto add_mutation = win.find("flat.add_mutation(node,");
        auto out_of_range_check = win.find(">= flat size");
        CHECK(node_resolve != std::string::npos, "AC6: node resolution present");
        CHECK(guard != std::string::npos, "AC6: guard present");
        CHECK(add_mutation != std::string::npos, "AC6: add_mutation present");
        CHECK(out_of_range_check != std::string::npos, "AC6: out-of-range check present");
        CHECK(node_resolve < guard, "AC6: guard after node resolve");
        CHECK(out_of_range_check < guard, "AC6: guard after out-of-range check");
        CHECK(guard < add_mutation, "AC6: add_mutation after guard");
    }

    std::println("\n=== #3131 record-patch hygiene: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_scalar_mutate_record_patch_hygiene();
}
#endif