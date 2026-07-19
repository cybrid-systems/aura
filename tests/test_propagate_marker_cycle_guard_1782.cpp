// @category: unit
// @reason: Issue #1782 — syntax:propagate-marker must terminate on
// cyclic FlatAST children (visited-set guard; parity #1679 / #1682).
//
//   AC1: source cites #1782; uses dense seen[] + no re-push on seen
//   AC2: A↔B cycle: terminates, marks each node once (count == 2)
//   AC3: self-loop: terminates, marks once
//   AC4: acyclic subtree: count > 1; wall time < 1s for cycle cases

#include "test_harness.hpp"

#include <chrono>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;
using clock = std::chrono::steady_clock;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

FlatAST* workspace(CompilerService& cs) {
    (void)cs.eval("(set-code \"(define seed 1)\")");
    (void)cs.eval("(eval-current)");
    return cs.workspace_flat();
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: source has visited-set guard (#1782) ---");
        auto src = read_first({"src/compiler/evaluator_primitives_compile_05.cpp",
                               "../src/compiler/evaluator_primitives_compile_05.cpp"});
        CHECK(!src.empty(), "read compile_05.cpp");
        CHECK(src.find("#1782") != std::string::npos, "cites #1782");
        auto pos = src.find("\"syntax:propagate-marker\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 2500);
        CHECK(win.find("seen") != std::string::npos, "uses seen[]");
        CHECK(win.find("seen[cix]") != std::string::npos, "skips already-seen children");
        CHECK(win.find("kMaxVisit") != std::string::npos, "has visit ceiling");
    }

    CompilerService cs;
    auto* flat = workspace(cs);
    CHECK(flat != nullptr, "workspace_flat");

    // ── AC4 / baseline: acyclic tree ──
    {
        std::println("\n--- AC4: acyclic subtree ---");
        // Use a real define subtree via Aura (no cycle).
        (void)cs.eval("(set-code \"(define (f x) (+ x 1))\")");
        (void)cs.eval("(eval-current)");
        auto r = cs.eval("(car (query:find \"f\"))");
        CHECK(r && is_int(*r), "find f");
        const auto root = as_int(*r);
        auto prop = cs.eval(std::format("(syntax:propagate-marker {} 1)", root));
        CHECK(prop && is_int(*prop), "propagate returns int");
        CHECK(as_int(*prop) > 1, "acyclic updates > 1 node");
    }

    // ── AC2: A↔B cycle ──
    {
        std::println("\n--- AC2: A↔B cycle terminates ---");
        flat = cs.workspace_flat();
        CHECK(flat != nullptr, "flat for cycle");
        auto lit = flat->add_literal(1);
        auto a = flat->add_if(lit, lit, lit);
        auto b = flat->add_if(lit, lit, lit);
        flat->set_child(a, 0, b);
        flat->set_child(b, 0, a);
        const auto t0 = clock::now();
        auto prop = cs.eval(std::format("(syntax:propagate-marker {} 1)", a));
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        CHECK(prop && is_int(*prop), "cycle propagate returns");
        CHECK(ms < 1000, std::format("cycle finished in {}ms", ms));
        // a + b + shared lit (cycle edges seen-skipped; no unbounded growth)
        const auto n = as_int(*prop);
        CHECK(n >= 2 && n <= 4, std::format("cycle marks finite nodes once (got {})", n));
        std::println("  cycle A↔B count={} in {}ms", n, ms);
    }

    // ── AC3: self-loop ──
    {
        std::println("\n--- AC3: self-loop terminates ---");
        flat = cs.workspace_flat();
        auto lit = flat->add_literal(0);
        auto s = flat->add_if(lit, lit, lit);
        flat->set_child(s, 1, s); // then-branch → self
        const auto t0 = clock::now();
        auto prop = cs.eval(std::format("(syntax:propagate-marker {} 2)", s));
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        CHECK(prop && is_int(*prop), "self-loop propagate returns");
        CHECK(ms < 1000, std::format("self-loop finished in {}ms", ms));
        // s itself + any non-self children (cond/else lit may be shared)
        // At minimum must mark s once and terminate; count is finite and small.
        CHECK(as_int(*prop) >= 1 && as_int(*prop) <= static_cast<std::int64_t>(flat->size()),
              std::format("self-loop count in range (got {})", as_int(*prop)));
        // Self edge must not re-mark: if we only push unseen, self is seen at root push
        // so then-child s is skipped. cond+else lit may add 1–2 more.
        CHECK(as_int(*prop) <= 3, std::format("self-loop no blow-up (got {})", as_int(*prop)));
        std::println("  self-loop count={} in {}ms", as_int(*prop), ms);
    }

    std::println("\n=== test_propagate_marker_cycle_guard_1782: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
