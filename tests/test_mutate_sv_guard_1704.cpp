// @category: unit
// @reason: Issue #1704 / #1705 — mutate:sv-add-coverpoint and
// mutate:sv-weaken-property use MutationBoundaryGuard + live-node
// check (no naked unlocked mutate). #1705 is the weaken sibling.
//
//   AC1: sv-add-coverpoint on live node succeeds and logs mutation
//   AC2: sv-weaken-property on live node succeeds and logs mutation (#1705)
//   AC3: out-of-range / dead id fails as #f
//   AC4: source has Guard + is_live_node + run_or_rollback for both prims

#include "test_harness.hpp"

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
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;

bool eval_ok(CompilerService& cs, const std::string& expr) {
    return cs.eval(expr).has_value();
}

bool find_last_op(const aura::ast::FlatAST& flat, std::string_view op,
                  aura::ast::MutationRecord* out) {
    auto view = flat.mutation_log_view();
    for (std::size_t i = view.size(); i > 0; --i) {
        if (view[i - 1].operator_name == op) {
            if (out)
                *out = view[i - 1];
            return true;
        }
    }
    return false;
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1: sv-add-coverpoint under Guard ──
    {
        std::println("\n--- AC1: sv-add-coverpoint Guard path ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (f x) x)\")")), "set-code");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat");
        aura::ast::NodeId target = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_live_node(id)) {
                target = id;
                break;
            }
        }
        CHECK(target != aura::ast::NULL_NODE, "found live target");
        const auto log_before = flat->mutation_count();

        auto r = cs.eval(std::string("(mutate:sv-add-coverpoint ") + std::to_string(target) +
                         " \"cp0\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "sv-add-coverpoint #t");
        CHECK(flat->mutation_count() > log_before, "mutation log grew");
        aura::ast::MutationRecord rec{};
        CHECK(find_last_op(*flat, "sv-add-coverpoint", &rec), "log has sv-add-coverpoint");
        CHECK(rec.target_node == target, "log target is covergroup id");
    }

    // ── AC2: sv-weaken-property under Guard ──
    {
        std::println("\n--- AC2: sv-weaken-property Guard path ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (g x) 1)\")")), "set-code g");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat ac2");
        aura::ast::NodeId target = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_live_node(id)) {
                target = id;
                break;
            }
        }
        CHECK(target != aura::ast::NULL_NODE, "found live target ac2");
        auto r = cs.eval(std::string("(mutate:sv-weaken-property ") + std::to_string(target) +
                         " \"rst_n\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "sv-weaken-property #t");
        aura::ast::MutationRecord rec{};
        CHECK(find_last_op(*flat, "sv-weaken-property", &rec), "log has sv-weaken-property");
        CHECK(rec.target_node == target, "log target is property id");
    }

    // ── AC3: bad id fails ──
    {
        std::println("\n--- AC3: out-of-range fails ---");
        CompilerService cs;
        CHECK((eval_ok(cs, "(set-code \"(define (h x) 0)\")")), "set-code h");
        auto* flat = cs.evaluator().workspace_flat();
        auto huge = static_cast<std::int64_t>(flat->size() + 9999);
        auto r1 =
            cs.eval(std::string("(mutate:sv-add-coverpoint ") + std::to_string(huge) + " \"cp\")");
        CHECK(r1 && is_bool(*r1) && !as_bool(*r1), "sv-add-coverpoint #f on bad id");
        auto r2 =
            cs.eval(std::string("(mutate:sv-weaken-property ") + std::to_string(huge) + " \"x\")");
        CHECK(r2 && is_bool(*r2) && !as_bool(*r2), "sv-weaken-property #f on bad id");
    }

    // ── AC4: source audit ──
    {
        std::println("\n--- AC4: source has #1704 Guard ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_mutate.cpp",
            "../src/compiler/evaluator_primitives_mutate.cpp",
        };
        std::string src;
        for (const char* p : candidates) {
            auto s = read_file(p);
            if (!s.empty()) {
                src = std::move(s);
                break;
            }
        }
        CHECK(!src.empty(), "read mutate.cpp");
        if (!src.empty()) {
            CHECK(src.find("Issue #1704") != std::string::npos, "cites #1704");
            CHECK(src.find("Issue #1705") != std::string::npos ||
                      src.find("#1705") != std::string::npos,
                  "cites #1705 on weaken sibling");
            // Both prims should construct MutationBoundaryGuard nearby.
            auto p1 = src.find("mutate:sv-add-coverpoint");
            auto p2 = src.find("mutate:sv-weaken-property");
            CHECK(p1 != std::string::npos && p2 != std::string::npos, "both prims present");
            if (p1 != std::string::npos) {
                auto win = src.substr(p1, 3500);
                CHECK(win.find("MutationBoundaryGuard") != std::string::npos,
                      "sv-add-coverpoint has Guard");
                CHECK(win.find("is_live_node") != std::string::npos,
                      "sv-add-coverpoint is_live_node");
                CHECK(win.find("run_or_rollback") != std::string::npos,
                      "sv-add-coverpoint run_or_rollback");
            }
            if (p2 != std::string::npos) {
                auto win = src.substr(p2, 3500);
                CHECK(win.find("MutationBoundaryGuard") != std::string::npos,
                      "sv-weaken-property has Guard");
                CHECK(win.find("is_live_node") != std::string::npos,
                      "sv-weaken-property is_live_node");
                CHECK(win.find("run_or_rollback") != std::string::npos,
                      "sv-weaken-property run_or_rollback");
                CHECK(win.find("#1705") != std::string::npos ||
                          win.find("1705") != std::string::npos,
                      "weaken path cites #1705");
            }
        }
    }

    std::println("\n=== test_mutate_sv_guard_1704: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
