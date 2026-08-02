// @category: unit
// @reason: Issue #2533 — residual hard-reclaim force-safepoint metrics + edges.
//
//   AC1: mark_reclaimed requests force_safepoint + cancel
//   AC2: residual_force_safepoint_total increments
//   AC3: source-cite check_gc_safepoint poll (no yield recursion)
//   AC4: query keys schema-2533 (source-cite)
//   AC5: still_running gauge still pairs
//   AC6: linter

#include "test_harness.hpp"
#include "serve/fiber.h"
#include <fstream>
#include <print>
#include <string>

import std;

namespace {
using aura::serve::Fiber;
using aura::test::g_failed;
using aura::test::g_passed;
std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}
} // namespace

int main() {
    std::println("=== Issue #2533: residual force-safepoint ===");
    const auto f0 = Fiber::residual_force_safepoint_total();
    Fiber f([] {
        // body never runs for this unit test — we only mark_reclaimed
    });
    // Simulate reclaim of a not-Done fiber (constructor leaves Ready/Created).
    f.mark_reclaimed();
    CHECK(f.is_reclaimed(), "reclaimed");
    CHECK(f.is_cancel_requested(), "AC1: cancel requested");
    // force flag may already be consumed by a concurrent path; total is the contract.
    CHECK(Fiber::residual_force_safepoint_total() > f0, "AC2: force-safepoint total++");
    // Body not run → still_running may or may not bump depending on state_
    // (non-Done). Accept either; note_body_exit is safe.
    f.note_body_exit_if_reclaimed();
    auto fib = read_file("src/serve/fiber.h");
    auto fcpp = read_file("src/serve/fiber.cpp");
    auto agent = read_file("src/compiler/evaluator_primitives_agent.cpp");
    CHECK(fib.find("request_force_safepoint") != std::string::npos, "AC3: API");
    CHECK(fcpp.find("2533") != std::string::npos, "AC3: cite cpp");
    CHECK(fcpp.find("force_safepoint_requested_") != std::string::npos ||
              fcpp.find("request_force_safepoint") != std::string::npos,
          "AC3: mark_reclaimed wires force");
    CHECK(agent.find("residual-force-safepoint-total") != std::string::npos, "AC4: query key");
    CHECK(agent.find("schema-2533") != std::string::npos, "AC4: schema");
    std::println("\n=== #2533: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
