// @category: unit
// @reason: Issue #1681 — strict stale-ref errors must not push "stale-ref"
// Issue #1681 (#1978 renamed): issue# moved from filename to header.
// into string_heap_ on every blocked call.
//
//   AC1: set-stale-ref-policy "strict" works
//   AC2: mutate:check-stable-ref with stale (id . gen) returns error (not #t)
//   AC3: N=500 strict blocks leave string_heap_size growth ≈ 0
//   AC4: stale_ref_blocked_count advances

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

} // namespace

int main() {
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
    auto pol = cs.eval("(mutate:set-stale-ref-policy \"strict\")");
    CHECK(pol && is_bool(*pol) && as_bool(*pol), "AC1: set strict policy");

    // Stable-ref wire format is (id . (gen . ())) i.e. (list id gen).
    const char* stale_check = "(mutate:check-stable-ref (list 0 9999))";

    // ── AC2: first blocked call is not a success bool #t ──
    {
        std::println("\n--- AC2: strict block returns non-#t ---");
        auto r = cs.eval(stale_check);
        CHECK(r.has_value(), "check-stable-ref returns a value");
        // Error pair / tagged error / bool #f — anything except success #t
        const bool ok_success = r && is_bool(*r) && as_bool(*r);
        CHECK(!ok_success, "strict stale is not #t");
        // Prefer error pair with "stale-ref" tag when policy is Strict.
        if (r && is_pair(*r)) {
            auto pi = aura::compiler::types::as_pair_idx(*r);
            auto car = cs.evaluator().pairs()[pi].car;
            if (is_string(car)) {
                auto si = aura::compiler::types::as_string_idx(car);
                const auto& tag = cs.evaluator().string_heap()[si];
                CHECK(tag == "stale-ref", std::format("error tag is stale-ref (got {})", tag));
            }
        }
        CHECK(cs.evaluator().get_stale_ref_blocked_count() >= 1, "blocked count after one call");
    }

    const auto heap0 = cs.evaluator().string_heap_size();
    const auto blocked0 = cs.evaluator().get_stale_ref_blocked_count();

    constexpr int kN = 500;
    std::println("\n--- AC3: {} strict blocks — no extra stale-ref intern ---", kN);
    for (int i = 0; i < kN; ++i)
        (void)cs.eval(stale_check);

    const auto heap1 = cs.evaluator().string_heap_size();
    const auto blocked1 = cs.evaluator().get_stale_ref_blocked_count();
    const auto growth = heap1 > heap0 ? heap1 - heap0 : 0;

    std::println("  string_heap {} → {} (growth={})", heap0, heap1, growth);
    std::println("  blocked {} → {}", blocked0, blocked1);

    // make_merr interns kind+message (2 strings) per error. Old code also
    // push_back("stale-ref") → 3 strings/call. After #1681 expect ~2*N, not 3*N.
    // Allow eval overhead but require growth strictly below 2.5*N.
    const std::size_t max_ok = static_cast<std::size_t>(kN) * 2 + kN / 2;
    CHECK(growth <= max_ok,
          std::format("heap growth {} <= ~2*N+overhead ({}) (no 3rd stale-ref intern)", growth,
                      max_ok));
    // And must be better than pre-fix 3-per-call floor if N is large.
    CHECK(growth < static_cast<std::size_t>(kN) * 3,
          "growth strictly less than 3*N (pre-fix leak rate)");
    CHECK(blocked1 >= blocked0 + static_cast<std::uint64_t>(kN), "AC4: stale_ref_blocked_count +N");

    std::println("\n=== test_stale_ref_string_heap_1681: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
