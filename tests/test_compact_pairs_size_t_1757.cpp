// @category: unit
// @reason: Issue #1757 — compact_pairs returns std::size_t (count
// semantic) matching compact_env_frames, not signed int64_t.
//
//   AC1: source cites #1757; return type is size_t
//   AC2: empty live_mask (all-live) returns size_t count
//   AC3: selective mask returns live count as size_t
//   AC4: all-dead returns 0 (size_t); remap entries -1
//   AC5: return type is not assignable-as-negative-error (unsigned)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <type_traits>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

using aura::compiler::Evaluator;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void alloc_pairs(aura::compiler::CompilerService& cs, int n) {
    for (int i = 0; i < n; ++i) {
        std::string src = "(cons " + std::to_string(i) + " " + std::to_string(i + 1) + ")";
        auto r = cs.eval(src);
        (void)r;
    }
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: #1757 size_t compact_pairs ---");
        std::string gc;
        for (const char* p :
             {"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"}) {
            gc = read_file(p);
            if (!gc.empty())
                break;
        }
        CHECK(!gc.empty(), "read evaluator_gc.cpp");
        CHECK(gc.find("#1757") != std::string::npos, "cites #1757");
        CHECK(gc.find("std::size_t Evaluator::compact_pairs") != std::string::npos,
              "impl returns size_t");

        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1757") != std::string::npos, "decl cites #1757");
        // Must not reintroduce signed return on the declaration line region.
        CHECK(ixx.find("std::size_t compact_pairs") != std::string::npos, "decl is size_t");
        CHECK(ixx.find("std::int64_t compact_pairs") == std::string::npos,
              "no int64_t compact_pairs decl");
    }

    // ── AC2: all-live empty mask ──
    {
        std::println("\n--- AC2: empty live_mask all-live size_t ---");
        aura::compiler::CompilerService cs;
        alloc_pairs(cs, 5);
        auto& ev = cs.evaluator();
        std::vector<bool> empty_mask;
        auto n = ev.compact_pairs(empty_mask);
        static_assert(std::is_same_v<decltype(n), std::size_t>,
                      "compact_pairs must return std::size_t");
        CHECK(n == 5, "5 pairs remain");
        CHECK(ev.resolve_pair(0) == 0, "identity remap");
        CHECK(ev.resolve_pair(4) == 4, "identity remap end");
    }

    // ── AC3: selective ──
    {
        std::println("\n--- AC3: selective live_mask size_t count ---");
        aura::compiler::CompilerService cs;
        alloc_pairs(cs, 5);
        auto& ev = cs.evaluator();
        std::vector<bool> mask = {true, false, true, false, true};
        std::size_t n = ev.compact_pairs(mask);
        CHECK(n == 3, "3 live pairs");
        CHECK(ev.resolve_pair(0) == 0, "old 0 → 0");
        CHECK(ev.resolve_pair(1) == -1, "old 1 dead");
        CHECK(ev.resolve_pair(2) == 1, "old 2 → 1");
        CHECK(ev.resolve_pair(4) == 2, "old 4 → 2");
    }

    // ── AC4: all-dead ──
    {
        std::println("\n--- AC4: all-dead returns 0 size_t ---");
        aura::compiler::CompilerService cs;
        alloc_pairs(cs, 3);
        auto& ev = cs.evaluator();
        std::vector<bool> mask = {false, false, false};
        std::size_t n = ev.compact_pairs(mask);
        CHECK(n == 0, "0 pairs remain");
        CHECK(ev.resolve_pair(0) == -1, "dead 0");
        CHECK(ev.resolve_pair(2) == -1, "dead 2");
    }

    // ── AC5: unsigned — no negative-error pattern ──
    {
        std::println("\n--- AC5: unsigned count (no < 0 error path) ---");
        static_assert(std::is_unsigned_v<std::size_t>, "size_t unsigned");
        using Ret = decltype(std::declval<Evaluator&>().compact_pairs(
            std::declval<const std::vector<bool>&>()));
        static_assert(std::is_same_v<Ret, std::size_t>, "return is size_t");
        static_assert(std::is_unsigned_v<Ret>, "return unsigned");
        CHECK(true, "return type is unsigned size_t");
    }

    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
