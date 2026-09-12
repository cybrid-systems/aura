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
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.root_remap_pass;
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
    // Issue #3648: the apply arms also consult the full window axes —
    // inject and restore them here. Defaults keep the historical
    // green-window behavior for existing callers (gate passes, falls
    // through to the LCP / remap checks).
    std::uint8_t prev_had;
    std::uint8_t prev_pin;
    std::uint8_t prev_incomplete;
    std::uint64_t prev_untracked;
    std::uint64_t prev_root_fail;
    const void* eval_id = nullptr;
    ProdDensifyWindowGuard(bool prod, std::uint64_t moved, bool lcp_allow,
                           const void* eval_id = nullptr, bool had_moving = true,
                           bool pin_held = true, bool incomplete = false,
                           std::uint64_t untracked = 0, std::uint64_t root_fail = 0)
        : eval_id(eval_id) {
        using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
        using aura::core::lifetime_consistency_proof::g_lcp_last_would_allow_commit;
        using aura::core::moving_densify_health::g_last_objects_moved;
        prev_prod = g_typed_mutation_audit_counters.production_defaults_active.load(
            std::memory_order_relaxed);
        prev_moved = g_last_objects_moved.load(std::memory_order_relaxed);
        prev_lcp = g_lcp_last_would_allow_commit().load(std::memory_order_relaxed);
        prev_had = aura::core::moving_densify_health::g_last_had_moving_densify.load(
            std::memory_order_relaxed);
        prev_pin = aura::core::moving_densify_health::g_last_pin_contract_held.load(
            std::memory_order_relaxed);
        prev_incomplete = aura::core::moving_densify_health::g_last_moving_incomplete_remap.load(
            std::memory_order_relaxed);
        prev_untracked = aura::core::moving_densify_health::g_last_untracked_kept.load(
            std::memory_order_relaxed);
        prev_root_fail = aura::core::moving_densify_health::g_last_root_remap_fail_total.load(
            std::memory_order_relaxed);
        g_typed_mutation_audit_counters.production_defaults_active.store(prod ? 1u : 0u,
                                                                         std::memory_order_relaxed);
        g_last_objects_moved.store(moved, std::memory_order_relaxed);
        g_lcp_last_would_allow_commit().store(lcp_allow ? 1 : 0, std::memory_order_relaxed);
        aura::core::moving_densify_health::g_last_had_moving_densify.store(
            had_moving ? 1 : 0, std::memory_order_relaxed);
        aura::core::moving_densify_health::g_last_pin_contract_held.store(
            pin_held ? 1 : 0, std::memory_order_relaxed);
        aura::core::moving_densify_health::g_last_moving_incomplete_remap.store(
            incomplete ? 1 : 0, std::memory_order_relaxed);
        aura::core::moving_densify_health::g_last_untracked_kept.store(untracked,
                                                                       std::memory_order_relaxed);
        aura::core::moving_densify_health::g_last_root_remap_fail_total.store(
            root_fail, std::memory_order_relaxed);
        // Issue #3634: keep the per-eval slot in sync with the fabricated
        // window — the apply arm consults the evaluator's own slot first
        // now, so `lcp_allow` must speak for that slot too.
        if (eval_id != nullptr) {
            namespace lcp = aura::core::lifetime_consistency_proof;
            auto p = lcp::make_lifetime_consistency_proof();
            p.would_allow_commit = lcp_allow;
            lcp::stamp_lifetime_consistency_proof_for(eval_id, p);
        }
    }
    ~ProdDensifyWindowGuard() {
        using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
        using aura::core::lifetime_consistency_proof::g_lcp_last_would_allow_commit;
        using aura::core::moving_densify_health::g_last_objects_moved;
        g_typed_mutation_audit_counters.production_defaults_active.store(prev_prod,
                                                                         std::memory_order_relaxed);
        g_last_objects_moved.store(prev_moved, std::memory_order_relaxed);
        g_lcp_last_would_allow_commit().store(prev_lcp, std::memory_order_relaxed);
        aura::core::moving_densify_health::g_last_had_moving_densify.store(
            prev_had, std::memory_order_relaxed);
        aura::core::moving_densify_health::g_last_pin_contract_held.store(
            prev_pin, std::memory_order_relaxed);
        aura::core::moving_densify_health::g_last_moving_incomplete_remap.store(
            prev_incomplete, std::memory_order_relaxed);
        aura::core::moving_densify_health::g_last_untracked_kept.store(prev_untracked,
                                                                       std::memory_order_relaxed);
        aura::core::moving_densify_health::g_last_root_remap_fail_total.store(
            prev_root_fail, std::memory_order_relaxed);
        if (eval_id != nullptr)
            aura::core::lifetime_consistency_proof::reset_lifetime_consistency_proof_for_test();
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
        ProdDensifyWindowGuard g(/*prod=*/false, /*moved=*/1, /*lcp_allow=*/false, &cs.evaluator());
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
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/false, &cs.evaluator());
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
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/0, /*lcp_allow=*/false, &cs.evaluator());
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
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/false, &cs.evaluator());
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
    ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/true, &cs.evaluator());
    auto got = cs.evaluator().apply_closure(cid, args);
    CHECK(!got.has_value(), "3469: apply_closure hard-refuses stale A");
    CHECK(m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed) == restamp0,
          "3469: no #2569 restamp");
    CHECK(m->closure_stale_returns.load(std::memory_order_relaxed) > stale0,
          "3469: reuses closure_stale_returns");
}

// Issue #3602: the FFI arm shares the #3421 densify-stale refuse. The TW/IR
// arms hard-refused; the FFI return path marshaled + called native without
// consulting last_object_remap_.
static void ac7_3602_ffi_densify_refuse() {
    std::println("\n--- #3602: FFI apply densify-stale refuse ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("production_ffi_apply_densify_hard_refuse") != std::string::npos,
          "3602: FFI refuse helper present");
    CHECK(flat.find("Issue #3602") != std::string::npos, "3602: eval_flat cites #3602");
    CHECK(flat.find("g_3602_") == std::string::npos, "3602: no invented g_3602_* counter");

    std::array<aura::compiler::types::EvalValue, 1> args{make_int(-5)};

    // AC-b: production + moved>0 + LCP deny -> FFI arm refuses (previously
    // the native call proceeded). No native call: refused abs returns no
    // value; closure_stale_returns reuses the #3421 counter.
    {
        CompilerService cs;
        auto* m = metrics_of(cs);
        auto cid_r = cs.eval("(require \"std/ffi\")");
        CHECK(cid_r.has_value(), "3602 AC-b: require std/ffi");
        cid_r = cs.eval("(c-func -1 \"abs\" \"(Int) -> Int\")");
        std::println("#3602 AC-b probe: has_value={} is_closure={} is_int={} int={}",
                     cid_r.has_value(), cid_r && is_closure(*cid_r), cid_r && is_int(*cid_r),
                     cid_r && is_int(*cid_r) ? as_int(*cid_r) : -999);
        CHECK(cid_r && is_closure(*cid_r), "3602 AC-b: c-func registered");
        const auto stale0 = m->closure_stale_returns.load(std::memory_order_relaxed);
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/false, &cs.evaluator());
        auto got = cs.evaluator().apply_closure(0, args);
        CHECK(!got.has_value(), "3602 AC-b: FFI apply hard-refuses under LCP deny");
        CHECK(m->closure_stale_returns.load(std::memory_order_relaxed) > stale0,
              "3602 AC-b: reuses closure_stale_returns");
    }

    // AC-c: production + moved>0 + LCP allow + opaque arg still a
    // last_object_remap_ key -> refuse (the #3602 residual: the marshalled
    // opaque value escaped slot rewrite - EXEMPT / observed-only /
    // pre-rewrite copy).
    {
        CompilerService cs;
        auto* m = metrics_of(cs);
        MovingFlagGuard on(1);
        auto& ar = cs.evaluator().test_arena();
        // test_root_remap_pass recipe: hole + stable slot so the window
        // relocates p and records old -> new in last_object_remap_.
        auto* p = ar.create<Pod16>(7, 8, 9, 10);
        auto* p1 = ar.create<Pod16>(11, 12, 13, 14);
        CHECK(p != nullptr && p1 != nullptr, "3602 AC-c: arena objects");
        void* old = p;
        aura::compiler::register_root_remap_stable_slot(&old);
        ar.destroy(p1);
        const auto r = ar.live_compact(LiveCompactMode::Moving);
        CHECK(!r.moving_blocked_precondition, "3602 AC-c: Moving not blocked");
        CHECK(ar.resolve_object_remap(old) != nullptr, "3602 AC-c: old is a remap key");
        auto cid_r = cs.eval("(require \"std/ffi\")");
        CHECK(cid_r.has_value(), "3602 AC-c: require std/ffi");
        cid_r = cs.eval("(c-func -1 \"abs\" \"(Opaque) -> Int\")");
        std::println("#3602 AC-c probe: has_value={} is_closure={} is_int={} int={}",
                     cid_r.has_value(), cid_r && is_closure(*cid_r), cid_r && is_int(*cid_r),
                     cid_r && is_int(*cid_r) ? as_int(*cid_r) : -999);
        CHECK(cid_r && is_closure(*cid_r), "3602 AC-c: opaque fn registered");
        auto op_r = cs.eval(std::format(
            "(c-opaque {})", static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(old))));
        CHECK(op_r && aura::compiler::types::is_opaque(*op_r), "3602 AC-c: stale opaque created");
        std::array<aura::compiler::types::EvalValue, 1> oargs{*op_r};
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/true, &cs.evaluator());
        const auto stale0 = m->closure_stale_returns.load(std::memory_order_relaxed);
        auto got = cs.evaluator().apply_closure(0, oargs);
        CHECK(!got.has_value(), "3602 AC-c: remap-key opaque arg refuses FFI call");
        CHECK(m->closure_stale_returns.load(std::memory_order_relaxed) > stale0,
              "3602 AC-c: reuses closure_stale_returns");
    }

    // AC-d: after a successful window, a live (rewritten / non-remap-key)
    // value applies -> native call proceeds under production.
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/ffi\")").has_value(), "3602 AC-d: require std/ffi");
        auto cid_r = cs.eval("(c-func -1 \"abs\" \"(Int) -> Int\")");
        CHECK(cid_r && is_closure(*cid_r), "3602 AC-d: c-func registered");
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/true, &cs.evaluator());
        auto got = cs.evaluator().apply_closure(0, args);
        CHECK(got.has_value() && is_int(*got) && as_int(*got) == 5,
              "3602 AC-d: apply allowed after rewrite (abs(-5)=5)");
    }

    // AC-e: Soft -> FFI path cost unchanged (production load only).
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/ffi\")").has_value(), "3602 AC-e: require std/ffi");
        auto cid_r = cs.eval("(c-func -1 \"abs\" \"(Int) -> Int\")");
        CHECK(cid_r && is_closure(*cid_r), "3602 AC-e: c-func registered");
        ProdDensifyWindowGuard g(/*prod=*/false, /*moved=*/1, /*lcp_allow=*/false, &cs.evaluator());
        auto got = cs.evaluator().apply_closure(0, args);
        CHECK(got.has_value() && is_int(*got) && as_int(*got) == 5,
              "3602 AC-e: Soft stays allowed");
    }

    // AC-f: production + objects_moved==0 -> quiet skip, apply proceeds.
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/ffi\")").has_value(), "3602 AC-f: require std/ffi");
        auto cid_r = cs.eval("(c-func -1 \"abs\" \"(Int) -> Int\")");
        CHECK(cid_r && is_closure(*cid_r), "3602 AC-f: c-func registered");
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/0, /*lcp_allow=*/false, &cs.evaluator());
        auto got = cs.evaluator().apply_closure(0, args);
        CHECK(got.has_value() && is_int(*got) && as_int(*got) == 5,
              "3602 AC-f: objects_moved==0 quiet skip");
    }
}

// Issue #3634: the apply arm (#3421 closure + #3602 FFI) consulted the
// PROCESS-WIDE LCP bit — one evaluator's Reject poisoned every other
// evaluator's apply until the next healthy stamp (#3617 fixed the steal
// arm only). Now: per-eval slot first (#3617), process-wide fallback for
// evaluators with no slot (today's fail-closed semantics preserved).
//
//   AC1: both predicates consult per-eval first with process-wide
//        fallback (source-cite below); process-wide stamp semantics
//        unchanged.
//   AC2: two evaluators — B stamps Allow, then A stamps Reject (the LAST
//        process-wide publish poisons the bit): A's apply still refuses,
//        B's apply proceeds while the process bit is poisoned, C (no
//        slot) falls back to the poisoned process bit and refuses.
//   AC3: quiet path unchanged — the slot probe sits after the
//        objects_moved quiet skip (Soft / no-move: two relaxed loads).
static void ac8_3634_per_eval_lcp_consult() {
    std::println("\n--- #3634: per-eval LCP consult on the apply arm ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("Issue #3634") != std::string::npos, "3634: predicates cite the issue");
    CHECK(flat.find("last_lifetime_consistency_proof_present_for") != std::string::npos,
          "3634 AC1: per-eval present probe consulted");
    CHECK(flat.find("last_lifetime_consistency_would_allow_for") != std::string::npos,
          "3634 AC1: per-eval verdict consulted");
    // AC3: the slot probe sits after the objects_moved quiet skip.
    const auto moved_pos = flat.find("g_last_objects_moved");
    const auto probe_pos = flat.find("last_lifetime_consistency_proof_present_for");
    CHECK(moved_pos != std::string::npos && probe_pos != std::string::npos && moved_pos < probe_pos,
          "3634 AC3: slot probe only after the objects_moved quiet skip");

    namespace lcp = aura::core::lifetime_consistency_proof;
    std::array<aura::compiler::types::EvalValue, 1> args{make_int(-5)};

    // B first: its healthy stamp is NOT the last process-wide publish.
    CompilerService cs_b;
    CHECK(cs_b.eval("(require \"std/ffi\")").has_value(), "3634 AC2: B require std/ffi");
    auto cid_b = cs_b.eval("(c-func -1 \"abs\" \"(Int) -> Int\")");
    CHECK(cid_b && is_closure(*cid_b), "3634 AC2: B c-func registered");
    const void* b_id = static_cast<const void*>(&cs_b.evaluator());
    lcp::stamp_lifetime_consistency_proof_for(b_id, lcp::make_lifetime_consistency_proof());

    // A second: its Reject stamp is the LAST process-wide publish — the
    // process bit is now poisoned for any evaluator without its own slot.
    CompilerService cs_a;
    CHECK(cs_a.eval("(require \"std/ffi\")").has_value(), "3634 AC2: A require std/ffi");
    auto cid_a = cs_a.eval("(c-func -1 \"abs\" \"(Int) -> Int\")");
    CHECK(cid_a && is_closure(*cid_a), "3634 AC2: A c-func registered");
    const void* a_id = static_cast<const void*>(&cs_a.evaluator());
    {
        auto p = lcp::make_lifetime_consistency_proof();
        p.would_allow_commit = false;
        lcp::stamp_lifetime_consistency_proof_for(a_id, p);
    }

    // C: fresh evaluator, never stamped (fallback face).
    CompilerService cs_c;
    CHECK(cs_c.eval("(require \"std/ffi\")").has_value(), "3634 AC2: C require std/ffi");
    auto cid_c = cs_c.eval("(c-func -1 \"abs\" \"(Int) -> Int\")");
    CHECK(cid_c && is_closure(*cid_c), "3634 AC2: C c-func registered");

    // Neutral window holder: a fresh evaluator binds the production window
    // WITHOUT touching the A/B/C per-eval LCP slots under test.
    CompilerService cs_win;
    ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/false, &cs_win.evaluator());

    // A: own slot = Reject → refuse (fail-closed preserved).
    auto ga = cs_a.evaluator().apply_closure(0, args);
    CHECK(!ga.has_value(), "3634 AC2: A (own Reject slot) still hard-refuses");

    // B: own slot = Allow → applies while the process bit is poisoned
    // (pre-#3634 this false-refused — the cross-evaluator leak).
    auto gb = cs_b.evaluator().apply_closure(0, args);
    CHECK(gb.has_value() && is_int(*gb) && as_int(*gb) == 5,
          "3634 AC2: B (own Allow slot) applies while process bit poisoned");

    // C: no slot → process-wide fallback (poisoned) → refuse.
    auto gc = cs_c.evaluator().apply_closure(0, args);
    CHECK(!gc.has_value(), "3634 AC2: C (no slot) falls back to the poisoned process bit");

    // Slot hygiene for later members in this process.
    lcp::reset_lifetime_consistency_proof_for_test();
}

// Issue #3648: production apply must also refuse on an incomplete densify
// window (untracked kept under objects_moved>0) even when the last LCP is
// still green and the stale flat misses last_object_remap_.
static void ac9_3648_window_gate_refuse() {
    std::println(
        "\n--- #3648 AC1/AC4: incomplete window refuses apply (LCP green, remap miss) ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("Issue #3648") != std::string::npos, "3648: eval_flat cites #3648");
    CHECK(flat.find("window_would_allow_mutate") != std::string::npos,
          "3648: apply consults the window gate");
    CHECK(flat.find("g_3648_") == std::string::npos, "3648: no invented g_3648_* counter");

    std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};
    // AC1: production + incomplete window (untracked kept ∧ moved>0) + LCP
    // green + flat NOT a remap key → hard-refuse, no #2569 restamp-eval.
    {
        CompilerService cs;
        auto* m = metrics_of(cs);
        const auto cid = make_stale_unimpacted_lambda(cs);
        const auto restamp0 = m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
        const auto stale0 = m->closure_stale_returns.load(std::memory_order_relaxed);
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/3, /*lcp_allow=*/true, &cs.evaluator(),
                                 /*had_moving=*/true, /*pin_held=*/true, /*incomplete=*/false,
                                 /*untracked=*/7, /*root_fail=*/0);
        auto got = cs.evaluator().apply_closure(cid, args);
        CHECK(!got.has_value(), "3648 AC1: incomplete window hard-refuses apply");
        CHECK(m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed) == restamp0,
              "3648 AC1: no #2569 restamp-eval");
        CHECK(m->closure_stale_returns.load(std::memory_order_relaxed) > stale0,
              "3648 AC4: reuses closure_stale_returns");
    }
    // AC1 flip: green window (untracked==0) + LCP green + remap miss → the
    // gate passes and #2569 recover still evaluates (no over-refusal).
    {
        CompilerService cs;
        const auto cid = make_stale_unimpacted_lambda(cs);
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/2, /*lcp_allow=*/true, &cs.evaluator(),
                                 /*had_moving=*/true, /*pin_held=*/true, /*incomplete=*/false,
                                 /*untracked=*/0, /*root_fail=*/0);
        auto got = cs.evaluator().apply_closure(cid, args);
        CHECK(got.has_value() && is_int(*got) && as_int(*got) == 2,
              "3648 AC1: green window does not refuse (gate passes)");
    }
}

// Issue #3648 AC2: green window + remap hit still refuses — the window gate
// is additive on top of the #3421/#3469 half-guards, not a replacement.
static void ac10_3648_green_window_remap_still_refuse() {
    std::println("\n--- #3648 AC2: green window + remap hit still refuses ---");
    std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};
    CompilerService cs;
    auto* m = metrics_of(cs);
    const auto cid0 = make_stale_unimpacted_lambda(cs);
    auto snap = cs.evaluator().find_active_closure(cid0);
    CHECK(snap.has_value(), "3648 AC2: live closure");
    if (!snap)
        return;
    MovingFlagGuard on(1);
    auto& ar = cs.evaluator().test_arena();
    auto* p0 = ar.create<Pod16>(1, 2, 3, 4);
    auto* p1 = ar.create<Pod16>(5, 6, 7, 8);
    CHECK(p0 && p1, "3648 AC2: tracked objects");
    void* A = p0;
    const auto r = ar.live_compact(LiveCompactMode::Moving);
    CHECK(!r.moving_blocked_precondition && r.objects_moved > 0, "3648 AC2: window moved");
    CHECK(ar.resolve_object_remap(A) != nullptr, "3648 AC2: A is a remap key");
    snap->flat = static_cast<decltype(snap->flat)>(A);
    snap->must_deopt_before_next_call = true;
    CHECK(cs.evaluator().erase_active_closure(cid0), "3648 AC2: erase original");
    const auto cid = cs.evaluator().register_active_closure(std::move(*snap));
    const auto restamp0 = m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
    const auto stale0 = m->closure_stale_returns.load(std::memory_order_relaxed);
    // Normalize the published window to green: the refuse must come from the
    // remap hit, not the window gate (proves the gate is additive).
    ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/true, &cs.evaluator(),
                             /*had_moving=*/true, /*pin_held=*/true, /*incomplete=*/false,
                             /*untracked=*/0, /*root_fail=*/0);
    auto got = cs.evaluator().apply_closure(cid, args);
    CHECK(!got.has_value(), "3648 AC2: remap hit refuses under green window");
    CHECK(m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed) == restamp0,
          "3648 AC2: no #2569 restamp");
    CHECK(m->closure_stale_returns.load(std::memory_order_relaxed) > stale0,
          "3648 AC2: reuses closure_stale_returns");
    (void)p1;
}

// Issue #3648 AC3: Soft / no-Moving / objects_moved==0 keep #2569 recover —
// the window gate sits behind the production and moved>0 quiet gates.
static void ac11_3648_soft_no_move_recover() {
    std::println("\n--- #3648 AC3: Soft / moved==0 / no-window recover unchanged ---");
    std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};
    {
        CompilerService cs;
        const auto cid = make_stale_unimpacted_lambda(cs);
        ProdDensifyWindowGuard g(/*prod=*/false, /*moved=*/3, /*lcp_allow=*/true, &cs.evaluator(),
                                 /*had_moving=*/true, /*pin_held=*/true, /*incomplete=*/false,
                                 /*untracked=*/9, /*root_fail=*/0);
        auto got = cs.evaluator().apply_closure(cid, args);
        CHECK(got.has_value() && is_int(*got) && as_int(*got) == 2,
              "3648 AC3: production off keeps #2569 recover");
    }
    {
        CompilerService cs;
        const auto cid = make_stale_unimpacted_lambda(cs);
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/0, /*lcp_allow=*/true, &cs.evaluator(),
                                 /*had_moving=*/true, /*pin_held=*/true, /*incomplete=*/false,
                                 /*untracked=*/9, /*root_fail=*/0);
        auto got = cs.evaluator().apply_closure(cid, args);
        CHECK(got.has_value() && is_int(*got) && as_int(*got) == 2,
              "3648 AC3: objects_moved==0 keeps #2569 recover");
    }
    {
        CompilerService cs;
        const auto cid = make_stale_unimpacted_lambda(cs);
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/3, /*lcp_allow=*/true, &cs.evaluator(),
                                 /*had_moving=*/false, /*pin_held=*/true, /*incomplete=*/false,
                                 /*untracked=*/9, /*root_fail=*/0);
        auto got = cs.evaluator().apply_closure(cid, args);
        CHECK(got.has_value() && is_int(*got) && as_int(*got) == 2,
              "3648 AC3: no Moving window (had=0) keeps recover");
    }
}

// Issue #3648 AC5: both apply arms consult the window gate (closure + FFI);
// suite wiring locked; no forbidden artifacts.
static void ac12_3648_wiring_and_family() {
    std::println("\n--- #3648 AC5: both arms wired; suite family ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    const auto begin = flat.find("static bool production_apply_closure_densify_hard_refuse");
    const auto end = flat.find("static void note_apply_closure_densify_hard_refuse", begin);
    CHECK(begin != std::string::npos && end != std::string::npos && end > begin,
          "3648 AC5: closure arm located");
    const auto closure_arm = flat.substr(begin, end - begin);
    CHECK(closure_arm.find("Issue #3648") != std::string::npos, "3648 AC5: closure arm cites");
    CHECK(closure_arm.find("window_would_allow_mutate") != std::string::npos,
          "3648 AC5: closure arm consults window gate");
    CHECK(closure_arm.find("g_last_untracked_kept") != std::string::npos,
          "3648 AC5: closure arm reads untracked axis");
    const auto moved_pos = flat.find("g_last_objects_moved", begin);
    const auto gate_rel = closure_arm.find("window_would_allow_mutate");
    CHECK(moved_pos != std::string::npos && gate_rel != std::string::npos &&
              moved_pos < begin + gate_rel,
          "3648 AC5: quiet moved-gate precedes window consult");
    const auto ffi_begin = flat.find("production_ffi_apply_densify_hard_refuse");
    const auto ffi_end = flat.find("// Issue #1511", ffi_begin);
    CHECK(ffi_begin != std::string::npos && ffi_end != std::string::npos && ffi_end > ffi_begin,
          "3648 AC5: FFI arm located");
    const auto ffi_body = flat.substr(ffi_begin, ffi_end - ffi_begin);
    CHECK(ffi_body.find("Issue #3648") != std::string::npos &&
              ffi_body.find("window_would_allow_mutate") != std::string::npos,
          "3648 AC5: FFI arm carries the window gate");
    const auto t = read_file("tests/compiler/test_setcode_rebind_survive.cpp");
    CHECK(t.find("ac9_3648_window_gate_refuse();") != std::string::npos &&
              t.find("ac10_3648_green_window_remap_still_refuse();") != std::string::npos &&
              t.find("ac11_3648_soft_no_move_recover();") != std::string::npos &&
              t.find("ac12_3648_wiring_and_family();") != std::string::npos,
          "3648 AC5: runner wired");
    const std::string issue_artifact = std::string("test_issue_") + "3648";
    CHECK(t.find(issue_artifact) == std::string::npos, "3648 AC5: no tests/issues file");
}

} // namespace

static void ac13_3678_ffi_pointer_class_refuse();
static void ac14_3681_production_pre_reemit_refuse();

int run_test_setcode_rebind_survive() {
    std::println("=== Issue #2569: set-code/rebind closure+hash survival ===");
    ac1_closure_survive();
    ac2_hash_survive();
    ac3_hash_ref_default();
    ac4_source_gate();
    ac5_3421_production_hard_refuse();
    ac6_3469_two_window_stale_flat_refuse();
    ac7_3602_ffi_densify_refuse();
    ac8_3634_per_eval_lcp_consult();
    // Issue #3648: apply also refuses on an incomplete densify window
    // (same gate as Phase-5); LCP-green + remap-miss no longer slips.
    ac9_3648_window_gate_refuse();
    ac10_3648_green_window_remap_still_refuse();
    ac11_3648_soft_no_move_recover();
    ac12_3648_wiring_and_family();
    ac13_3678_ffi_pointer_class_refuse();
    // Issue #3681: production refuses MustDeopt/dirty-stale wash onto the
    // pre-reemit body; unimpacted rebinds keep the #2569 recover.
    ac14_3681_production_pre_reemit_refuse();
    std::println("\n=== #2569/#3421/#3469/#3602/#3634/#3648: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

// Issue #3678: the FFI refuse joins EVERY pointer-class register — not only
// the Opaque arm. The Int arm (default / unknown codes) passes raw values
// through, so a JIT / escaped native addr stashed as an int rides into the
// native call unchecked (UAF in native, not in apply_closure).
static void ac13_3678_ffi_pointer_class_refuse() {
    std::println("\n--- #3678: FFI refuse joins every pointer-class register ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("Issue #3678") != std::string::npos, "3678: eval_flat cites #3678");
    CHECK(flat.find("every pointer-class register joins the refuse") != std::string::npos,
          "3678: args loop checks every pointer-class register (source-cite)");

    // AC1: production + moved>0 + window green + LCP allow + FFI arg declared
    // Int carrying a remap-key value (escaped opaque copy) -> apply refuses;
    // closure_stale_returns bumps. No native call.
    {
        CompilerService cs;
        auto* m = metrics_of(cs);
        MovingFlagGuard on(1);
        auto& ar = cs.evaluator().test_arena();
        auto* p = ar.create<Pod16>(7, 8, 9, 10);
        auto* p1 = ar.create<Pod16>(11, 12, 13, 14);
        CHECK(p != nullptr && p1 != nullptr, "3678 AC1: arena objects");
        void* old = p;
        aura::compiler::register_root_remap_stable_slot(&old);
        ar.destroy(p1);
        const auto r = ar.live_compact(LiveCompactMode::Moving);
        CHECK(!r.moving_blocked_precondition, "3678 AC1: Moving not blocked");
        CHECK(ar.resolve_object_remap(old) != nullptr, "3678 AC1: old is a remap key");
        CHECK(cs.eval("(require \"std/ffi\")").has_value(), "3678 AC1: require std/ffi");
        auto cid_r = cs.eval("(c-func -1 \"abs\" \"(Int) -> Int\")");
        CHECK(cid_r && is_closure(*cid_r), "3678 AC1: int fn registered");
        std::array<aura::compiler::types::EvalValue, 1> iargs{
            make_int(static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(old)))};
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/true, &cs.evaluator());
        const auto stale0 = m->closure_stale_returns.load(std::memory_order_relaxed);
        auto got = cs.evaluator().apply_closure(0, iargs);
        CHECK(!got.has_value(), "3678 AC1: remap-key Int arg refuses FFI call");
        CHECK(m->closure_stale_returns.load(std::memory_order_relaxed) > stale0,
              "3678 AC1: reuses closure_stale_returns");
    }

    // AC2: a true libc-heap ptr that is NOT a remap key stays EXEMPT — no
    // pin, no refuse: the native call proceeds under production.
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/ffi\")").has_value(), "3678 AC2: require std/ffi");
        auto cid_r = cs.eval("(c-func -1 \"abs\" \"(Int) -> Int\")");
        CHECK(cid_r && is_closure(*cid_r), "3678 AC2: int fn registered");
        void* libc = std::malloc(32);
        CHECK(libc != nullptr, "3678 AC2: libc buffer allocated");
        std::array<aura::compiler::types::EvalValue, 1> largs{
            make_int(static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(libc)))};
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/1, /*lcp_allow=*/true, &cs.evaluator());
        auto got = cs.evaluator().apply_closure(0, largs);
        CHECK(got.has_value(), "3678 AC2: non-key libc ptr applies (EXEMPT, no pin)");
        std::free(libc);
    }

    // Soft: no extra refuse — the same remap-key Int arg without the
    // production face applies (Soft/Off observe-only contract).
    {
        CompilerService cs;
        CHECK(cs.eval("(require \"std/ffi\")").has_value(), "3678 Soft: require std/ffi");
        auto cid_r = cs.eval("(c-func -1 \"abs\" \"(Int) -> Int\")");
        CHECK(cid_r && is_closure(*cid_r), "3678 Soft: int fn registered");
        MovingFlagGuard on(1);
        auto& ar = cs.evaluator().test_arena();
        auto* p = ar.create<Pod16>(1, 2, 3, 4);
        auto* p1 = ar.create<Pod16>(5, 6, 7, 8);
        void* old = p;
        aura::compiler::register_root_remap_stable_slot(&old);
        ar.destroy(p1);
        const auto r = ar.live_compact(LiveCompactMode::Moving);
        CHECK(ar.resolve_object_remap(old) != nullptr, "3678 Soft: old is a remap key");
        std::array<aura::compiler::types::EvalValue, 1> iargs{
            make_int(static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(old)))};
        ProdDensifyWindowGuard g(/*prod=*/false, /*moved=*/1, /*lcp_allow=*/true, &cs.evaluator());
        auto got = cs.evaluator().apply_closure(0, iargs);
        CHECK(got.has_value(), "3678 Soft: no extra refuse without production face");
    }
}

// ── Issue #3681: production apply_closure refuses pre-reemit body ──
// MustDeopt / epoch-stale on a closure whose define was dirtied this
// epoch must not wash into eval_flat of the pre-reemit body under
// production defaults (#3421 does not fire without a densify window).
// Unimpacted rebind of *other* defines keeps the #2569/#2578 recover.
static void ac14_3681_production_pre_reemit_refuse() {
    std::println("\n--- #3681: production MustDeopt/dirty refuse; unimpacted recover ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("Issue #3681") != std::string::npos, "3681: apply arms cite #3681");
    const auto h3421 = flat.find("production_apply_closure_densify_hard_refuse(arena_, cl_copy,");
    const auto g3681 = flat.find("typed_audit::production_defaults_active()", h3421);
    CHECK(h3421 != std::string::npos && g3681 != std::string::npos && g3681 > h3421,
          "3681 AC3: #3681 gates ordered after the #3421 refuse");
    CHECK(flat.find("is_define_dirty_fn_") != std::string::npos,
          "3681 AC5: reuses the facade-published dirty surface (no second table)");

    std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};

    // AC1: production + set-body of F (define dirtied this epoch) + live
    // named closure of F → apply refuses the pre-reemit body; no #2569
    // restamp wash.
    {
        CompilerService cs;
        auto* m = metrics_of(cs);
        CHECK(cs.eval("(define (f3681 x) (* x 10))").has_value(), "AC1: define F");
        auto cf = cs.eval("f3681");
        CHECK(cf && is_closure(*cf), "AC1: closure of F");
        const auto cid_f = as_closure_id(*cf);
        auto pre = cs.evaluator().apply_closure(cid_f, args);
        CHECK(pre.has_value() && is_int(*pre) && as_int(*pre) == 10,
              "AC1 pre: F applies the current body before set-body");
        CHECK(cs.eval("(mutate:set-body \"f3681\" \"(lambda (x) (* x 20))\")").has_value(),
              "AC1: set-body F (define dirtied this epoch)");
        const auto restamp0 = m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/0, /*lcp_allow=*/true, &cs.evaluator());
        auto got = cs.evaluator().apply_closure(cid_f, args);
        CHECK(!got.has_value(), "AC1: production refuses pre-reemit body of dirtied F");
        CHECK(m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed) == restamp0,
              "AC1: no #2569 restamp wash");
    }

    // AC2: production + rebind of OTHER define G — closure of unimpacted
    // named F still recovers onto its (still-current) body.
    {
        CompilerService cs;
        auto* m = metrics_of(cs);
        CHECK(cs.eval("(define (g3681f x) (* x 10))").has_value(), "AC2: define F (unimpacted)");
        CHECK(cs.eval("(define (g3681g x) (* x 1))").has_value(), "AC2: define G (rebind target)");
        auto cf = cs.eval("g3681f");
        CHECK(cf && is_closure(*cf), "AC2: closure of F");
        const auto cid_f = as_closure_id(*cf);
        CHECK(cs.evaluator().apply_closure(cid_f, args).has_value(), "AC2 pre: F applies");
        ProdDensifyWindowGuard g(/*prod=*/true, /*moved=*/0, /*lcp_allow=*/true, &cs.evaluator());
        CHECK(cs.eval("(mutate:rebind \"g3681g\" \"(lambda (x) (* x 2))\" \"t\")").has_value(),
              "AC2: rebind other define G");
        CHECK(cs.eval("(eval-current)").has_value(), "AC2: eval-current after rebind");
        const auto restamp0 = m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
        auto got = cs.evaluator().apply_closure(cid_f, args);
        CHECK(got.has_value() && is_int(*got) && as_int(*got) == 10,
              "AC2: unimpacted F still recovers + runs its current body");
        CHECK(m->live_closure_epoch_restamp_total.load(std::memory_order_relaxed) >= restamp0,
              "AC2: #2569 restamp allowed for unimpacted define");
    }

    // AC5: Soft/Off — same set-body scenario keeps the soft-recover.
    {
        CompilerService cs;
        CHECK(cs.eval("(define (s3681 x) (* x 10))").has_value(), "AC5: define F (Soft)");
        auto cf = cs.eval("s3681");
        CHECK(cf && is_closure(*cf), "AC5: closure of F");
        const auto cid_f = as_closure_id(*cf);
        CHECK(cs.eval("(mutate:set-body \"s3681\" \"(lambda (x) (* x 30))\")").has_value(),
              "AC5: set-body F (Soft, dirty)");
        auto got = cs.evaluator().apply_closure(cid_f, args);
        CHECK(got.has_value(), "AC5: Soft keeps the soft-recover (no production refuse)");
    }
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_setcode_rebind_survive();
}
#endif
