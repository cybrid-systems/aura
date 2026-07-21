// @category: unit
// @reason: Issue #1746 — mutation_boundary_depth_slot must key by
// Issue #1746 (#1978 renamed): issue# moved from filename to header.
// Evaluator::instance_id_, not raw address (free-list reuse UAF).
//
//   AC1: source cites #1746; map keyed by uint64_t / instance_id
//   AC2: successive Evaluators get distinct instance_id values
//   AC3: concurrent Evaluators have independent depth slots
//   AC4: nested MutationBoundaryGuard still LIFO on one Evaluator

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: depth_slot keys by instance_id ---");
        std::string fib;
        for (const char* p : {"src/compiler/evaluator_fiber_mutation.cpp",
                              "../src/compiler/evaluator_fiber_mutation.cpp"}) {
            fib = read_file(p);
            if (!fib.empty())
                break;
        }
        CHECK(!fib.empty(), "read evaluator_fiber_mutation.cpp");
        CHECK(fib.find("#1746") != std::string::npos, "cites #1746");
        CHECK(fib.find("instance_id()") != std::string::npos, "reads instance_id()");
        CHECK(fib.find("unordered_map<std::uint64_t, int>") != std::string::npos ||
                  fib.find("unordered_map<uint64_t, int>") != std::string::npos,
              "map keyed by uint64_t");
        // Old address-key form must be gone from the slot body.
        auto pos = fib.find("mutation_boundary_depth_slot(Evaluator* ev)");
        CHECK(pos != std::string::npos, "found depth_slot definition");
        if (pos != std::string::npos) {
            auto win = fib.substr(pos, 900);
            CHECK(win.find("unordered_map<Evaluator*, int>") == std::string::npos,
                  "no longer keyed by Evaluator*");
        }

        std::string ctor;
        for (const char* p :
             {"src/compiler/evaluator_ctor.cpp", "../src/compiler/evaluator_ctor.cpp"}) {
            ctor = read_file(p);
            if (!ctor.empty())
                break;
        }
        CHECK(!ctor.empty() && ctor.find("instance_id_") != std::string::npos,
              "ctor assigns instance_id_");
        CHECK(ctor.find("#1746") != std::string::npos, "ctor cites #1746");
    }

    // ── AC2: distinct instance ids ──
    {
        std::println("\n--- AC2: successive Evaluators get distinct ids ---");
        Evaluator e1;
        Evaluator e2;
        Evaluator e3;
        CHECK(e1.instance_id() != 0, "e1 id non-zero");
        CHECK(e2.instance_id() != 0, "e2 id non-zero");
        CHECK(e3.instance_id() != 0, "e3 id non-zero");
        CHECK(e1.instance_id() != e2.instance_id(), "e1 != e2");
        CHECK(e2.instance_id() != e3.instance_id(), "e2 != e3");
        CHECK(e1.instance_id() != e3.instance_id(), "e1 != e3");
    }

    // ── AC3: independent depth slots across Evaluators ──
    {
        std::println("\n--- AC3: independent depth slots ---");
        CompilerService cs1;
        CompilerService cs2;
        auto& e1 = cs1.evaluator();
        auto& e2 = cs2.evaluator();
        CHECK(e1.instance_id() != e2.instance_id(), "cs evaluators distinct ids");
        CHECK(e1.mutation_boundary_depth_slot_value() == 0, "e1 idle depth 0");
        CHECK(e2.mutation_boundary_depth_slot_value() == 0, "e2 idle depth 0");

        bool ok1 = true;
        {
            Evaluator::MutationBoundaryGuard g1(e1, &ok1);
            CHECK(e1.mutation_boundary_depth_slot_value() == 1, "e1 depth 1 under guard");
            CHECK(e2.mutation_boundary_depth_slot_value() == 0, "e2 still 0 while e1 held");

            bool ok2 = true;
            Evaluator::MutationBoundaryGuard g2(e2, &ok2);
            CHECK(e2.mutation_boundary_depth_slot_value() == 1, "e2 depth 1 under own guard");
            CHECK(e1.mutation_boundary_depth_slot_value() == 1, "e1 still 1 with e2 also held");
        }
        CHECK(e1.mutation_boundary_depth_slot_value() == 0, "e1 back to 0");
        CHECK(e2.mutation_boundary_depth_slot_value() == 0, "e2 back to 0");
    }

    // ── AC4: nested guards LIFO on one Evaluator ──
    {
        std::println("\n--- AC4: nested guards LIFO ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        bool ok = true;
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "start depth 0");
        {
            Evaluator::MutationBoundaryGuard outer(ev, &ok);
            CHECK(ev.mutation_boundary_depth_slot_value() == 1, "outer depth 1");
            {
                Evaluator::MutationBoundaryGuard inner(ev, &ok);
                CHECK(ev.mutation_boundary_depth_slot_value() == 2, "inner depth 2");
            }
            CHECK(ev.mutation_boundary_depth_slot_value() == 1, "after inner, depth 1");
        }
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "after outer, depth 0");
    }

    std::println("\n=== test_depth_slot_instance_id_1746: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
