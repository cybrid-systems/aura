// @category: unit
// @reason: Issue #1745 — AURA_MUTATION_BOUNDARY_PROTECT must not use a
// Issue #1745 (#1978 renamed): issue# moved from filename to header.
// bare `break` (switch-context footgun); if/else only.
//
//   AC1: source cites #1745; production macro has no bare break
//   AC2: fixed if/else pattern runs BODY when try_acquire succeeds
//   AC3: fixed pattern is safe inside a switch case
//   AC4: failed acquire skips BODY
//
// Note: the production #define lives in evaluator.ixx (module IU) and is
// not re-exported to importers; behavioral ACs exercise the same fixed
// expansion pattern used by the production macro (#1745 Option B).

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

// Mirror of post-#1745 AURA_MUTATION_BOUNDARY_PROTECT (if/else, no break).
#define TEST_MBP_PROTECT(EV, BODY)                                                                 \
    do {                                                                                           \
        bool _aura_mbp_ok = true;                                                                  \
        auto _aura_mbp_gr = ::aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(       \
            (EV), /*pending_count=*/1, &_aura_mbp_ok);                                             \
        if (_aura_mbp_gr) {                                                                        \
            auto& _aura_mbp_guard = **_aura_mbp_gr;                                                \
            (void)_aura_mbp_guard;                                                                 \
            BODY;                                                                                  \
        } else {                                                                                   \
            _aura_mbp_ok = false;                                                                  \
        }                                                                                          \
        (void)_aura_mbp_ok;                                                                        \
    } while (0)

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string macro_window(const std::string& src) {
    auto pos = src.find("#define AURA_MUTATION_BOUNDARY_PROTECT");
    if (pos == std::string::npos)
        return {};
    return src.substr(pos, 1200);
}

} // namespace

int main() {
    // ── AC1: production source — no bare break ──
    {
        std::println("\n--- AC1: macro has no bare break ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1745") != std::string::npos, "cites #1745");
        auto win = macro_window(ixx);
        CHECK(!win.empty(), "found AURA_MUTATION_BOUNDARY_PROTECT");
        CHECK(win.find("try_acquire") != std::string::npos, "uses try_acquire");
        CHECK(win.find("break;") == std::string::npos, "no bare break in macro body");
        CHECK(win.find("else") != std::string::npos, "uses if/else reject path");
    }

    // ── AC2: BODY runs on successful acquire ──
    {
        std::println("\n--- AC2: BODY runs when acquire succeeds ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        int ran = 0;
        TEST_MBP_PROTECT(ev, ++ran);
        CHECK(ran == 1, "BODY executed once");
    }

    // ── AC3: safe inside switch — later cases still reachable ──
    {
        std::println("\n--- AC3: switch case does not swallow later cases ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        int hit_replace = 0;
        int hit_skip = 0;
        enum class Op { Replace, Skip };
        for (Op op : {Op::Replace, Op::Skip}) {
            switch (op) {
                case Op::Replace:
                    TEST_MBP_PROTECT(ev, ++hit_replace);
                    break;
                case Op::Skip:
                    ++hit_skip;
                    break;
            }
        }
        CHECK(hit_replace == 1, "Replace case body ran");
        CHECK(hit_skip == 1, "Skip case still reached after protect in Replace");
    }

    // ── AC4: failed acquire skips BODY ──
    {
        std::println("\n--- AC4: reject skips BODY ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_resource_quota_mutations(1);
        ev.reset_mutation_quota_used();
        int first = 0;
        int second = 0;
        TEST_MBP_PROTECT(ev, ++first);
        CHECK(first == 1, "first BODY ran within budget");
        const auto rej0 = ev.get_mutation_guard_try_acquire_reject_total();
        TEST_MBP_PROTECT(ev, ++second);
        CHECK(second == 0, "BODY skipped on quota reject");
        CHECK(ev.get_mutation_guard_try_acquire_reject_total() > rej0,
              "try_acquire reject metric bumped");
        ev.set_resource_quota_mutations(0);
    }

    std::println("\n=== test_mbp_macro_no_break_1745: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
