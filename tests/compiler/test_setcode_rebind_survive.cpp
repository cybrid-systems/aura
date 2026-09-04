// @category: unit
// @reason: Issue #2569 — set-code / mutate:rebind must not kill unimpacted
//          closures or hash telemetry (Aether closed-loop agent state).
//
//   AC1: define bump + set-code seed + N rebind rounds — bump stays callable
//   AC2: hash-ref/hash-set! survive rebind with correct values
//   AC3: (hash-ref h k default) honors default (IR 3-arg form)
//   AC4: source-cite + cmake + gate

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"
#include "core/lifetime_consistency_proof.hh"
#include "core/moving_densify_health.hh"

#include <array>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.arena;

namespace {

using aura::ast::LiveCompactMode;
using aura::ast::set_moving_compact_enabled;
using aura::compiler::Closure;
using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

struct Pod16 {
    std::int32_t a = 0, b = 0, c = 0, d = 0;
    Pod16() = default;
    Pod16(std::int32_t a_, std::int32_t b_, std::int32_t c_, std::int32_t d_) noexcept
        : a(a_)
        , b(b_)
        , c(c_)
        , d(d_) {}
};

struct MovingFlagGuard {
    int prev = -1;
    explicit MovingFlagGuard(int enable) {
        prev = aura::ast::moving_compact_enabled();
        set_moving_compact_enabled(enable);
    }
    ~MovingFlagGuard() { set_moving_compact_enabled(prev); }
};

// Issue #3421: inject production + last densify window, restore on scope exit.
struct ProdDensifyWindowGuard {
    std::uint32_t prev_prod;
    std::uint64_t prev_moved;
    std::uint8_t prev_lcp;
    ProdDensifyWindowGuard(bool prod, std::uint64_t moved, bool lcp_allow) {
        using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
        using aura::core::lifetime_consistency_proof::g_lcp_last_would_allow_commit;
        using aura::core::moving_densify_health::g_last_objects_moved;
        prev_prod = g_typed_mutation_audit_counters.production_defaults_active.load(
            std::memory_order_relaxed);
        prev_moved = g_last_objects_moved.load(std::memory_order_relaxed);
        prev_lcp = g_lcp_last_would_allow_commit().load(std::memory_order_relaxed);
        g_typed_mutation_audit_counters.production_defaults_active.store(prod ? 1u : 0u,
                                                                         std::memory_order_relaxed);
        g_last_objects_moved.store(moved, std::memory_order_relaxed);
        g_lcp_last_would_allow_commit().store(lcp_allow ? 1 : 0, std::memory_order_relaxed);
    }
    ~ProdDensifyWindowGuard() {
        using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
        using aura::core::lifetime_consistency_proof::g_lcp_last_would_allow_commit;
        using aura::core::moving_densify_health::g_last_objects_moved;
        g_typed_mutation_audit_counters.production_defaults_active.store(prev_prod,
                                                                         std::memory_order_relaxed);
        g_last_objects_moved.store(prev_moved, std::memory_order_relaxed);
        g_lcp_last_would_allow_commit().store(prev_lcp, std::memory_order_relaxed);
    }
};

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

// Unimpacted lambda + rebind of a *different* define so #2569 fallback fires.
static ClosureId make_stale_unimpacted_lambda(CompilerService& cs) {
    CHECK(cs.eval("(require \"std/mutate\" all:)").has_value(), "require mutate");
    CHECK(cs.eval("(define score (lambda (x) (* x 2)))").has_value(), "define score");
    auto r = cs.eval("(lambda (x) (+ x 1))");
    CHECK(r && is_closure(*r), "unimpacted lambda");
    CHECK(cs.eval("(mutate:rebind \"score\" \"(lambda (x) (* x 3))\" \"t\")").has_value(),
          "rebind other define");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after rebind");
    return r && is_closure(*r) ? as_closure_id(*r) : 0;
}

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static bool eval_int_eq(CompilerService& cs, std::string_view e, std::int64_t n) {
    auto r = cs.eval(e);
    return r && is_int(*r) && as_int(*r) == n;
}

static void ac1_closure_survive() {
    std::println("\n--- #2569 AC1: closures survive set-code + rebind ---");
    CompilerService cs;
    CHECK(cs.eval("(require \"std/mutate\" all:)").has_value(), "require mutate");
    CHECK(cs.eval("(define box (list 0))").has_value(), "define box");
    CHECK(cs.eval("(define bump (lambda () (set-car! box (+ (car box) 1)) (car box)))").has_value(),
          "define bump");
    CHECK(eval_int_eq(cs, "(bump)", 1), "bump #1");
    CHECK(cs.eval("(set-code \"(define score (lambda (x) (* x 2)))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    CHECK(eval_int_eq(cs, "(bump)", 2), "bump after set-code");
    CHECK(eval_int_eq(cs, "(score 5)", 10), "score *2");
    CHECK(cs.eval("(mutate:rebind \"score\" \"(lambda (x) (* x 3))\" \"t\")").has_value(),
          "rebind *3");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after rebind");
    auto b3 = cs.eval("(bump)");
    std::println("  restamp_total={}", cs.evaluator().get_live_closure_epoch_restamp_total());
    std::println("  bump after rebind: void={} int={}", b3 && is_void(*b3),
                 b3 && is_int(*b3) ? as_int(*b3) : -99);
    CHECK(b3 && is_int(*b3) && as_int(*b3) == 3, "AC1: bump after rebind");
    CHECK(eval_int_eq(cs, "(score 5)", 15), "score *3");
    // N rounds without invalid closure
    for (int i = 0; i < 4; ++i) {
        CHECK(cs.eval("(mutate:rebind \"score\" \"(lambda (x) (+ x 1))\" \"t\")").has_value(),
              "rebind loop");
        CHECK(cs.eval("(eval-current)").has_value(), "eval loop");
        auto br = cs.eval("(bump)");
        CHECK(br && is_int(*br), "AC1: bump survives rebind round");
    }
}

static void ac2_hash_survive() {
    std::println("\n--- #2569 AC2: hash telemetry survives rebind ---");
    CompilerService cs;
    CHECK(cs.eval("(require \"std/mutate\" all:)").has_value(), "require");
    CHECK(cs.eval("(define *h* (hash \"rounds\" 0 \"commits\" 0))").has_value(), "hash");
    CHECK(cs.eval("(define (hbump key) (hash-set! *h* key (+ 1 (hash-ref *h* key 0))) "
                  "(hash-ref *h* key 0))")
              .has_value(),
          "hbump");
    CHECK(eval_int_eq(cs, "(hbump \"rounds\")", 1), "hbump rounds 1");
    CHECK(cs.eval("(set-code \"(define score (lambda (x) (* x 2)))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(eval_int_eq(cs, "(hbump \"rounds\")", 2), "hbump after set-code");
    CHECK(cs.eval("(mutate:rebind \"score\" \"(lambda (x) (* x 3))\" \"t\")").has_value(),
          "rebind");
    CHECK(cs.eval("(eval-current)").has_value(), "eval rebind");
    CHECK(eval_int_eq(cs, "(hbump \"commits\")", 1), "AC2: hbump commits after rebind");
    CHECK(eval_int_eq(cs, "(hash-ref *h* \"rounds\" 0)", 2), "AC2: rounds value intact");
}

static void ac3_hash_ref_default() {
    std::println("\n--- #2569 AC3: hash-ref default (3-arg IR form) ---");
    CompilerService cs;
    CHECK(cs.eval("(define *h* (hash \"a\" 1))").has_value(), "hash");
    CHECK(eval_int_eq(cs, "(hash-ref *h* \"a\")", 1), "2-arg hit");
    CHECK(eval_int_eq(cs, "(hash-ref *h* \"a\" 99)", 1), "3-arg hit keeps value");
    CHECK(eval_int_eq(cs, "(hash-ref *h* \"missing\" 99)", 99), "3-arg miss returns default");
}

static void ac4_source_gate() {
    std::println("\n--- #2569 AC4: source-cite + gate ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#2569") != std::string::npos, "AC4: eval_flat cites #2569");
    CHECK(flat.find("soft-recover") != std::string::npos ||
              flat.find("soft_recover") != std::string::npos,
          "AC4: soft-recover unimpacted closures");
    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("#2569") != std::string::npos, "AC4: lowering cites #2569");
    CHECK(low.find("hash-ref") != std::string::npos, "AC4: hash-ref IR fix");
    const auto vec = read_file("src/compiler/evaluator_primitives_vector.cpp");
    CHECK(vec.find("#2569") != std::string::npos, "AC4: hash-ref default");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_setcode_rebind_survive") != std::string::npos, "AC4: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_setcode_rebind_2569") != std::string::npos, "AC4: check script");
    CHECK(build.find("cmd_setcode_rebind_coverage") != std::string::npos, "AC4: gate cmd");
}

static void ac5_3421_production_hard_refuse() {
    std::println("\n--- #3421 AC1/AC2: production densify-stale hard-refuse; Soft recover ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("kApplyClosureDensifyHardRefuseIssue = 3421") != std::string::npos,
          "3421 helper stamp");
    CHECK(flat.find("production_apply_closure_densify_hard_refuse") != std::string::npos,
          "3421 helper");
    CHECK(flat.find("resolve_object_remap") != std::string::npos, "3421 consults remap");
    CHECK(flat.find("last_lifetime_consistency_would_allow") != std::string::npos,
          "3421 consults LCP");
    CHECK(flat.find("g_last_objects_moved") != std::string::npos, "3421 last-window moved");
    CHECK(flat.find("g_3421_") == std::string::npos, "no invented g_3421_* counter");

    std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};

    // AC2 Soft: production off + moved + LCP deny → #2569 recover still allowed.
    {
        CompilerService cs;
        auto* m = metrics_of(cs);
        const auto cid = make_stale_unimpacted_lambda(cs);
        const auto restamp0 = m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
        ProdDensifyWindowGuard g(/*prod=*/false, /*moved=*/1, /*lcp_allow=*/false);
        auto got = cs.evaluator().apply_closure(cid, args);
        CHECK(got.has_value() && is_int(*got) && as_int(*got) == 2,
              "AC2 Soft: #2569 recover still allowed");
        CHECK(m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed) >= restamp0,
              "AC2 Soft restamp may grow");
    }

    // AC1 production + objects_moved>0 + LCP deny → hard-refuse, no restamp.
    {
        CompilerService cs;
        auto* m = metrics_of(cs);
        const auto cid = make_stale_unimpacted_lambda(cs);
        const auto restamp0 = m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
        const auto stale0 = m->closure_stale_returns.load(std::memory_order_relaxed);
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/false);
        auto got = cs.evaluator().apply_closure(cid, args);
        CHECK(!got.has_value(), "AC1 production densify-stale hard-refuse");
        CHECK(m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed) == restamp0,
              "AC1 must not #2569 restamp");
        CHECK(m->closure_stale_returns.load(std::memory_order_relaxed) > stale0,
              "AC1 reuses closure_stale_returns");
    }

    // AC2 production + objects_moved==0 → #2569 recover (quiet skip of remap/LCP).
    {
        CompilerService cs;
        auto* m = metrics_of(cs);
        const auto cid = make_stale_unimpacted_lambda(cs);
        const auto restamp0 = m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/0, /*lcp_allow=*/false);
        auto got = cs.evaluator().apply_closure(cid, args);
        CHECK(got.has_value() && is_int(*got) && as_int(*got) == 2,
              "AC2 production + objects_moved==0 still recover");
        CHECK(m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed) >= restamp0,
              "AC2 no-move restamp allowed");
    }

    // Soak: production refuse stays refuse across rounds (no apply on densify-old).
    {
        CompilerService cs;
        auto* m = metrics_of(cs);
        const auto cid = make_stale_unimpacted_lambda(cs);
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/false);
        const auto restamp0 = m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
        for (int i = 0; i < 8; ++i) {
            auto got = cs.evaluator().apply_closure(cid, args);
            CHECK(!got.has_value(), "3421 soak: refuse holds");
        }
        CHECK(m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed) == restamp0,
              "3421 soak: no restamp");
    }
}

// Issue #3469: two Moving windows + LCP allow + closure still holding
// window-1 flat → hard-refuse (fold keeps A as a remap key).
static void ac6_3469_two_window_stale_flat_refuse() {
    std::println("\n--- #3469: two-window densify stale flat hard-refuse ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(flat.find("Issue #3469") != std::string::npos, "3469: eval_flat cites #3469");
    CHECK(arena.find("prev_remap") != std::string::npos, "3469: fold previous remap");
    CHECK(flat.find("g_3469_") == std::string::npos, "3469: no invented g_3469_*");

    std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};
    CompilerService cs;
    auto* m = metrics_of(cs);
    const auto cid0 = make_stale_unimpacted_lambda(cs);
    auto snap = cs.evaluator().find_active_closure(cid0);
    CHECK(snap.has_value(), "3469: live closure");
    if (!snap)
        return;

    MovingFlagGuard on(1);
    auto& ar = cs.evaluator().test_arena();
    auto* p0 = ar.create<Pod16>(1, 2, 3, 4);
    auto* p1 = ar.create<Pod16>(5, 6, 7, 8);
    auto* p2 = ar.create<Pod16>(9, 10, 11, 12);
    CHECK(p0 && p1 && p2, "3469: tracked objects");
    void* A = p0;
    const auto r1 = ar.live_compact(LiveCompactMode::Moving);
    CHECK(!r1.moving_blocked_precondition && r1.objects_moved > 0, "3469: window 1 moved");
    void* B = ar.resolve_object_remap(A);
    CHECK(B != nullptr, "3469: A remapped in window 1");
    const auto r2 = ar.live_compact(LiveCompactMode::Moving);
    CHECK(!r2.moving_blocked_precondition && r2.objects_moved > 0, "3469: window 2 moved");
    CHECK(ar.resolve_object_remap(A) != nullptr, "3469: A still a remap key");

    snap->flat = static_cast<decltype(snap->flat)>(A);
    snap->must_deopt_before_next_call = true;
    CHECK(cs.evaluator().erase_active_closure(cid0), "3469: erase original");
    const auto cid = cs.evaluator().register_active_closure(std::move(*snap));
    const auto restamp0 = m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
    const auto stale0 = m->closure_stale_returns.load(std::memory_order_relaxed);
    // LCP allow: the residual is remap-miss after a healthy window 2.
    ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/true);
    auto got = cs.evaluator().apply_closure(cid, args);
    CHECK(!got.has_value(), "3469: apply_closure hard-refuses stale A");
    CHECK(m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed) == restamp0,
          "3469: no #2569 restamp");
    CHECK(m->closure_stale_returns.load(std::memory_order_relaxed) > stale0,
          "3469: reuses closure_stale_returns");
}

} // namespace

int run_test_setcode_rebind_survive() {
    std::println("=== Issue #2569: set-code/rebind closure+hash survival ===");
    ac1_closure_survive();
    ac2_hash_survive();
    ac3_hash_ref_default();
    ac4_source_gate();
    ac5_3421_production_hard_refuse();
    ac6_3469_two_window_stale_flat_refuse();
    std::println("\n=== #2569/#3421/#3469: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_setcode_rebind_survive();
}
#endif
