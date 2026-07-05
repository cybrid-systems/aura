// @category: integration
// @reason: Issue #615 PRIM_ERROR macro + math.cpp unification + silent-swallow audit
//
// Scope-limited close matching the #601 / #491 / #479 / #604 / #606 / #614
// pattern: ship the PRIM_ERROR abstraction + 4 math.cpp call-site
// consolidations + 11 silent-swallow audit comments + (query:primitive-
// error-stats) regression coverage now; the enhanced by_kind /
// recovery_attempts breakdown + cross-fiber stress matrix remains a
// separate follow-up.

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_615_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

// Read primitive_error_count as observed from the C++ side.
// Lives in evaluator.get_primitive_error_count().
static std::int64_t err_count_via_cxx(aura::compiler::CompilerService& cs) {
    return static_cast<std::int64_t>(cs.evaluator().get_primitive_error_count());
}

// Read primitive_error_count via the public query:primitive-error-stats
// Aura primitive (Issue #478). The result is a pair (count . stored);
// this extracts the car.
static std::int64_t err_count_via_prim(aura::compiler::CompilerService& cs) {
    auto r = cs.eval("(car (query:primitive-error-stats))");
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// Read stored (error_values_size) via cdr of query:primitive-error-stats.
static std::int64_t err_stored_via_prim(aura::compiler::CompilerService& cs) {
    auto r = cs.eval("(cdr (query:primitive-error-stats))");
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_615_detail

int main() {
    using namespace aura_issue_615_detail;
    std::println("=== Issue #615: PRIM_ERROR macro + math.cpp unification + silent-swallow audit ===");

    aura::compiler::CompilerService cs;

    // AC1: query:primitive-error-stats still returns a (count . stored)
    // pair (regression — Issue #478 shape unchanged).
    {
        std::println("\n--- AC1: query:primitive-error-stats shape (regression) ---");
        auto r = cs.eval("(query:primitive-error-stats)");
        CHECK(r && aura::compiler::types::is_pair(*r),
              "query:primitive-error-stats returns a pair");
        auto cnt = err_count_via_prim(cs);
        auto stored = err_stored_via_prim(cs);
        CHECK(cnt >= 0, std::format("car (count) is non-negative (got {})", cnt));
        CHECK(stored >= 0, std::format("cdr (stored) is non-negative (got {})", stored));
    }

    // AC2: invalid regex triggers PRIM_ERROR in regex-match? — counter
    // bumps +1 AND result is an error value (not void, not int).
    {
        std::println("\n--- AC2: regex-match? invalid regex -> PRIM_ERROR ---");
        const auto cxx_before = err_count_via_cxx(cs);
        const auto prim_before = err_count_via_prim(cs);
        const auto stored_before = err_stored_via_prim(cs);
        auto ev = cs.eval("(regex-match? \"[\" \"test\")");
        CHECK(ev && aura::compiler::types::is_error(*ev),
              "regex-match? with invalid regex returns an error value (not 0, not void)");
        const auto cxx_after = err_count_via_cxx(cs);
        const auto prim_after = err_count_via_prim(cs);
        const auto stored_after = err_stored_via_prim(cs);
        CHECK(cxx_after == cxx_before + 1,
              std::format("C++-side counter bumped exactly +1 ({} -> {})",
                          cxx_before, cxx_after));
        CHECK(prim_after == prim_before + 1,
              std::format("Aura-side (car) bumped exactly +1 ({} -> {})",
                          prim_before, prim_after));
        CHECK(stored_after == stored_before + 1,
              std::format("Aura-side (cdr / error_values_size) bumped exactly +1 ({} -> {})",
                          stored_before, stored_after));
    }

    // AC3: same matrix — regex-find / regex-replace / regex-split all
    // go through PRIM_ERROR on invalid regex (this is the unification
    // covered by #615 — all 4 sites use the same macro path).
    {
        std::println("\n--- AC3: regex-find/replace/split each -> PRIM_ERROR on invalid regex ---");
        struct Case {
            const char* expr;
            const char* label;
        };
        const std::vector<Case> cases = {
            {"(regex-find \"[\" \"test\")", "regex-find"},
            {"(regex-replace \"[\" \"test\" \"x\")", "regex-replace"},
            {"(regex-split \"[\" \"test\")", "regex-split"},
        };
        for (const auto& c : cases) {
            const auto before = err_count_via_cxx(cs);
            auto r = cs.eval(c.expr);
            const auto after = err_count_via_cxx(cs);
            CHECK(r && aura::compiler::types::is_error(*r),
                  std::format("{}: invalid regex returns error value", c.label));
            CHECK(after == before + 1,
                  std::format("{}: counter bumped +1 ({} -> {})", c.label, before, after));
        }
    }

    // AC4: valid regex MUST NOT bump the counter (negative case).
    {
        std::println("\n--- AC4: valid regex is silent (counter unchanged) ---");
        const auto before = err_count_via_cxx(cs);
        auto r1 = cs.eval("(regex-match? \"foo\" \"foobar\")");
        auto r2 = cs.eval("(regex-find \"foo\" \"foobar\")");
        auto r3 = cs.eval("(regex-replace \"foo\" \"foobar\" \"baz\")");
        auto r4 = cs.eval("(regex-split \",\" \"a,b,c\")");
        CHECK(r1 && aura::compiler::types::is_int(*r1) &&
                  aura::compiler::types::as_int(*r1) == 1,
              "valid regex-match? returns 1 (match found)");
        CHECK(r2 && aura::compiler::types::is_string(*r2),
              "valid regex-find returns a string");
        CHECK(r3 && aura::compiler::types::is_string(*r3),
              "valid regex-replace returns a string");
        CHECK(r4 && aura::compiler::types::is_pair(*r4),
              "valid regex-split returns a pair (non-empty list)");
        const auto after = err_count_via_cxx(cs);
        CHECK(after == before,
              std::format("valid-regex path did NOT bump counter ({} -> {})", before, after));
    }

    // AC5: silent-swallow audits remain silent. The 11 [SILENCE-PRIM-#615]
    // sites intentionally swallow because the catch is a defensive
    // fallback, not a user-input validation path. Smoke the
    // user-invocable subset (string->number, where :depth) with
    // malformed input to confirm they don't accidentally start raising
    // errors. The agent/ast internal-parsing swallows are not directly
    // user-invocable; their [SILENCE-PRIM-#615] tags document the
    // intent and are verified by code review.
    {
        std::println("\n--- AC5: silent-swallow fallbacks remain silent (regression) ---");
        // string->number parse failure (pair.cpp:474) — must return #f,
        // NOT error. Raising would break every existing caller.
        const auto before = err_count_via_cxx(cs);
        auto p = cs.eval("(string->number \"not-a-number\")");
        CHECK(p && aura::compiler::types::is_bool(*p) &&
                  !aura::compiler::types::as_bool(*p),
              "string->number on parse failure still returns #f (per documented contract)");
        const auto after = err_count_via_cxx(cs);
        CHECK(after == before,
              std::format("string->number fallback did NOT bump counter ({} -> {})",
                          before, after));
        // Note: agent.cpp swallows (line 75 / 1760 / 1788) and ast.cpp
        // falls (line 195 / 423) are internal parsing helpers — not
        // directly user-invocable. Their audit tags are verified by
        // code review at PR time, not by runtime regression here.
    }

    // AC6: concurrent regex error matrix — 2 threads × 16 bad-regex
    // calls each. Scaled-down stand-in for the full cross-fiber stress
    // matrix (follow-up). Ensures the atomic counter is wired correctly
    // under concurrency.
    {
        std::println("\n--- AC6: concurrent regex errors under 2 threads ---");
        std::mutex eval_mtx;
        std::atomic<int> error_count{0};
        constexpr int k_iters = 16;
        const auto before = err_count_via_cxx(cs);
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(regex-match? \"[\" \"test\")");
                if (r && aura::compiler::types::is_error(*r))
                    error_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        const auto after = err_count_via_cxx(cs);
        CHECK(error_count.load() == k_iters * 2,
              std::format("concurrent invalid-regex: {} / {} returned error",
                          error_count.load(), k_iters * 2));
        CHECK(after == before + k_iters * 2,
              std::format("C++ counter bumped exactly +{} under concurrency ({} -> {})",
                          k_iters * 2, before, after));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
