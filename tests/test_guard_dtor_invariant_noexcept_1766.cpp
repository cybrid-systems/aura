// @category: unit
// @reason: Issue #1766 — ~MutationBoundaryGuard must not leak depth
// via throwing ensure_* probes. Contract: depth decremented first;
// ensure_mutation_invariants / hygiene / arena probes are noexcept.
//
//   AC1: source cites #1766; ensure_mutation_invariants is noexcept
//   AC2: dtor decrements depth slot before ensure_mutation_invariants
//   AC3: hygiene + arena probes are noexcept in declaration
//   AC4: Guard happy path leaves depth at 0 and does not throw

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <type_traits>

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

std::string dtor_window(const std::string& src) {
    auto pos = src.find("~MutationBoundaryGuard()");
    if (pos == std::string::npos)
        return {};
    // Span the full dtor body through ensure_* / hygiene / arena probes.
    auto end = src.find("unique_lock destructor runs automatically", pos);
    if (end == std::string::npos)
        end = src.find("MutationBoundaryGuard(const MutationBoundaryGuard&) = delete", pos);
    if (end == std::string::npos || end <= pos)
        end = pos + 16000;
    return src.substr(pos, end - pos);
}

} // namespace

int main() {
    // ── AC1/AC2/AC3: source contract ──
    {
        std::println("\n--- AC1/AC2/AC3: dtor order + noexcept probes ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1766") != std::string::npos, "cites #1766");
        CHECK(ixx.find("void ensure_mutation_invariants() noexcept") != std::string::npos,
              "ensure_mutation_invariants is noexcept");
        CHECK(ixx.find("ensure_hygiene_violation_detection() const noexcept") != std::string::npos,
              "hygiene probe is noexcept");
        CHECK(ixx.find("probe_arena_auto_policy_on_boundary_exit(bool success) noexcept") !=
                  std::string::npos,
              "arena probe is noexcept");

        auto win = dtor_window(ixx);
        CHECK(!win.empty(), "found dtor window");
        // Actual source: int prev = (*slot)--;
        const auto prev_dec = win.find("(*slot)--");
        const auto ensure_pos = win.find("ensure_mutation_invariants()");
        CHECK(prev_dec != std::string::npos, "depth slot decrement in dtor");
        CHECK(ensure_pos != std::string::npos, "ensure_mutation_invariants in dtor");
        CHECK(prev_dec < ensure_pos, "depth decremented before ensure_mutation_invariants");

        std::string fib;
        for (const char* p : {"src/compiler/evaluator_fiber_mutation.cpp",
                              "../src/compiler/evaluator_fiber_mutation.cpp"}) {
            fib = read_file(p);
            if (!fib.empty())
                break;
        }
        CHECK(!fib.empty() && fib.find("#1766") != std::string::npos, "impl cites #1766");

        // Type-level noexcept check.
        static_assert(noexcept(std::declval<Evaluator&>().ensure_mutation_invariants()),
                      "ensure_mutation_invariants must be noexcept");
        static_assert(
            noexcept(std::declval<const Evaluator&>().ensure_hygiene_violation_detection()),
            "ensure_hygiene_violation_detection must be noexcept");
        CHECK(true, "static_assert noexcept on ensure_*");
    }

    // ── AC4: happy path ──
    {
        std::println("\n--- AC4: Guard happy path depth returns to 0 ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "start depth 0");
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard g(ev, &ok);
            CHECK(ok, "guard ok");
            CHECK(ev.mutation_boundary_depth_slot_value() == 1, "depth 1 under guard");
            // Direct probe must not throw.
            ev.ensure_mutation_invariants();
            ev.ensure_hygiene_violation_detection();
        }
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0 after dtor");
        CHECK(ok, "success flag still true");
    }

    std::println("\n=== test_guard_dtor_invariant_noexcept_1766: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
