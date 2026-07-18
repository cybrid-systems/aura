// @category: unit
// @reason: Issue #1699 — mutate:splice re-validates parent after EACH
// parse_to_flat in the multi-arg loop (6th capture-before-parse instance;
// #1690 wired fail-hard; this locks multi-arg locality + documents that
// is_valid_in must NOT be used post-parse because parse_to_flat restamps gens).
//
//   AC1: multi-arg splice after pad growth inserts under intended parent
//   AC2: all returned child ids are live children of that parent
//   AC3: bad parent id fails
//   AC4: source has per-iteration re-validate with is_live_node (public+lockless)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::test::g_failed;
using aura::test::g_passed;

bool eval_ok(CompilerService& cs, const std::string& expr) {
    return cs.eval(expr).has_value();
}

bool child_of(const aura::ast::FlatAST& flat, aura::ast::NodeId parent, aura::ast::NodeId child) {
    if (parent >= flat.size() || !flat.is_live_node(parent))
        return false;
    for (auto c : flat.children(parent))
        if (c == child)
            return true;
    return false;
}

// Collect int car values from a proper list result (splice returns list of ids).
std::vector<std::int64_t> list_ints(CompilerService& cs,
                                    const aura::compiler::types::EvalValue& v) {
    std::vector<std::int64_t> out;
    auto cur = v;
    while (is_pair(cur)) {
        auto idx = as_pair_idx(cur);
        auto& pairs = cs.evaluator().pairs();
        if (idx >= pairs.size())
            break;
        if (is_int(pairs[idx].car))
            out.push_back(as_int(pairs[idx].car));
        cur = pairs[idx].cdr;
    }
    return out;
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1/AC2: pad + multi-arg splice under Begin ──
    {
        std::println("\n--- AC1/AC2: pad + multi-arg splice locality ---");
        CompilerService cs;
        std::string src = "(define (f x) (begin 1))";
        for (int i = 0; i < 48; ++i) {
            src += " (define (pad";
            src += std::to_string(i);
            src += " y) (+ y ";
            src += std::to_string(i);
            src += "))";
        }
        CHECK((eval_ok(cs, std::string("(set-code \"") + src + "\")")), "set-code pad");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat");

        aura::ast::NodeId begin_id = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_live_node(id) && flat->get(id).tag == aura::ast::NodeTag::Begin) {
                begin_id = id;
                break;
            }
        }
        CHECK(begin_id != aura::ast::NULL_NODE, "found Begin parent");
        const auto child_count_before = flat->children(begin_id).size();
        const auto size_before = flat->size();

        // Three code strings with growth pressure between iterations.
        auto expr = std::string("(mutate:splice ") + std::to_string(begin_id) +
                    " 0 \"(+ 10 20 30 40 50)\" \"(* 2 3 4 5 6)\" \"(- 9 8 7 6 5)\" \"1699-ac1\")";
        auto r = cs.eval(expr);
        CHECK(r.has_value(), "splice eval ok");
        CHECK(flat->size() > size_before, "flat grew after multi-parse");
        CHECK(flat->is_live_node(begin_id), "Begin parent still live");
        CHECK(flat->children(begin_id).size() >= child_count_before + 1,
              "Begin gained at least one child");

        if (r) {
            auto ids = list_ints(cs, *r);
            std::println("  splice returned {} id(s)", ids.size());
            CHECK(ids.size() >= 1, "at least one inserted id returned");
            int under = 0;
            for (auto raw : ids) {
                auto nid = static_cast<aura::ast::NodeId>(raw);
                if (flat->is_live_node(nid) && child_of(*flat, begin_id, nid))
                    ++under;
            }
            CHECK(under == static_cast<int>(ids.size()),
                  "every returned id is a live child of intended Begin");
        }

        // Pads intact (wrong parent set_child would corrupt them).
        auto p0 = cs.eval("(pad0 1)");
        (void)cs.eval("(eval-current)");
        p0 = cs.eval("(pad0 1)");
        CHECK(p0.has_value(), "pad0 still evaluates");
    }

    // ── AC3: bad parent ──
    {
        std::println("\n--- AC3: bad parent id ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (g x) (begin 0))\")")), "set-code g");
        auto* flat = cs.evaluator().workspace_flat();
        auto huge = static_cast<std::int64_t>(flat->size() + 9999);
        auto r = cs.eval(std::string("(mutate:splice ") + std::to_string(huge) +
                         " 0 \"1\" \"2\" \"bad\")");
        bool failed = !r;
        if (r && is_int(*r))
            failed = false; // unexpected success as single int
        // merr is typically a pair / non-list of ints
        if (r) {
            auto ids = list_ints(cs, *r);
            if (!ids.empty())
                failed = false;
            else
                failed = true;
        }
        CHECK(failed, "bad parent does not return inserted id list");
    }

    // ── AC4: source audit ──
    {
        std::println("\n--- AC4: source has #1699 parent_ref re-validate ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_mutate.cpp",
            "src/compiler/evaluator_eval_flat.cpp",
            "../src/compiler/evaluator_primitives_mutate.cpp",
            "../src/compiler/evaluator_eval_flat.cpp",
        };
        std::string mutate_src, flat_src;
        for (const char* p : candidates) {
            auto s = read_file(p);
            if (s.empty())
                continue;
            if (std::string_view(p).find("evaluator_primitives_mutate") != std::string_view::npos)
                mutate_src = std::move(s);
            if (std::string_view(p).find("evaluator_eval_flat") != std::string_view::npos)
                flat_src = std::move(s);
        }
        CHECK(!mutate_src.empty(), "read mutate.cpp");
        CHECK(!flat_src.empty(), "read eval_flat.cpp");
        if (!mutate_src.empty()) {
            CHECK(mutate_src.find("Issue #1699") != std::string::npos, "public cites #1699");
            CHECK(mutate_src.find("splice: parent invalid after parse") != std::string::npos,
                  "public stale-ref message");
            CHECK(mutate_src.find("restamp") != std::string::npos ||
                      mutate_src.find("is_valid_in") != std::string::npos,
                  "public documents restamp / is_valid_in pitfall");
            // Must not require is_valid_in after parse in the splice loop.
            // Heuristic: the stale-ref branch uses is_live_node(parent).
            CHECK(mutate_src.find("!flat.is_live_node(parent)") != std::string::npos ||
                      mutate_src.find("!flat.is_live_node(parent))") != std::string::npos,
                  "public splice uses is_live_node after parse");
        }
        if (!flat_src.empty()) {
            CHECK(flat_src.find("Issue #1699") != std::string::npos, "lockless cites #1699");
            CHECK(flat_src.find("batch :splice: parent invalid after parse") != std::string::npos,
                  "lockless stale message");
            CHECK(flat_src.find("!flat.is_live_node(parent)") != std::string::npos,
                  "lockless splice uses is_live_node after parse");
        }
    }

    std::println("\n=== test_mutate_splice_stale_parent_1699: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
