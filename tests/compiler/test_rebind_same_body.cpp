// @category: unit
// @reason: Issue #5147 — mutate:rebind of one define must not drop JIT
//          freshness for the rest of the workspace. A byte-identical body
//          is a no-op (no dirty, no bridge bump). A real body change drops
//          native code for that define and its callers only.
//
//   AC1: same source, whitespace, and define-vs-lambda form leave the
//        bridge epoch, the walk-stale counter, and the re-lower counters
//        unchanged, and the old results stay
//   AC2: a real body change (small and over the 48-node light cap) updates
//        the define and its caller, and still does not bump the bridge
//        epoch or walk_active_closures
//   AC3: an untouched define keeps its value across both

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

struct Snap {
    std::uint64_t bridge = 0;
    std::uint64_t walk = 0;
    std::uint64_t relower = 0;
    std::uint64_t body = 0;
    std::uint64_t epoch = 0;
};

Snap snap_of(CompilerService& cs) {
    Snap s;
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    if (m != nullptr) {
        s.bridge = m->bridge_epoch_bumps_total.load(std::memory_order_relaxed);
        s.walk = m->jit_walk_active_closures_stale_total.load(std::memory_order_relaxed);
        s.relower = m->relower_full_called_count.load(std::memory_order_relaxed);
        s.body = m->cascade_body_only_count.load(std::memory_order_relaxed);
    }
    s.epoch = cs.evaluator().current_bridge_epoch();
    return s;
}

std::string snap_text(const Snap& a, const Snap& b) {
    return std::format("bridge {}->{} walk {}->{} epoch {}->{} relower {}->{} body {}->{}",
                       a.bridge, b.bridge, a.walk, b.walk, a.epoch, b.epoch, a.relower, b.relower,
                       a.body, b.body);
}

// Begin of 60 refs is over the 48-node light-path cap, so this define
// takes the full dirty path. The value is the last expression.
std::string wide_src(bool plus_one) {
    std::string b = "(define (wide x) (begin";
    for (int i = 0; i < 60; ++i)
        b += " x";
    if (plus_one)
        b += " (+ x 1)";
    b += "))";
    return b;
}

std::string workspace_src() {
    std::string s;
    s += "(define (hot n) (let loop ((i 0) (acc 0)) ";
    s += "(if (>= i n) acc (loop (+ i 1) (+ acc (cold i)))))) ";
    s += "(define (cold i) (+ i 1)) ";
    s += "(define (idle) 7) ";
    s += wide_src(false);
    return s;
}

void check_eval(CompilerService& cs, std::string_view expr, const char* label) {
    auto r = cs.eval(expr);
    if (!r.has_value()) {
        CHECK(false, std::format("{}: {}", label, r.error().message));
        return;
    }
    CHECK(true, label);
}

void check_int(CompilerService& cs, std::string_view expr, std::int64_t want, const char* label) {
    auto r = cs.eval(expr);
    if (!r.has_value()) {
        CHECK(false, std::format("{}: {}", label, r.error().message));
        return;
    }
    if (!is_int(*r)) {
        CHECK(false, std::format("{}: not an int", label));
        return;
    }
    CHECK(as_int(*r) == want, std::format("{}: got {} want {}", label, as_int(*r), want));
}

bool rebind(CompilerService& cs, std::string_view name, std::string_view code, const char* why) {
    const auto expr = std::format("(mutate :rebind \"{}\" \"{}\" \"{}\")", name, code, why);
    auto r = cs.eval(expr);
    if (!r.has_value()) {
        std::println(std::cerr, "  rebind {}: {}", why, r.error().message);
        return false;
    }
    return is_bool(*r) && as_bool(*r);
}

void ac5147_same_body_keeps_bridge() {
    std::println("\n--- #5147: same-body rebind does not walk the workspace ---");
    CompilerService cs;
    check_eval(cs, "(require \"std/mutate\" all:)", "require mutate");
    check_eval(cs, std::format("(set-code \"{}\")", workspace_src()), "set-code");
    check_eval(cs, "(eval-current)", "eval-current");
    // (hot 3) = (0+1) + (1+1) + (2+1)
    check_int(cs, "(hot 3)", 6, "hot before");
    check_int(cs, "(cold 4)", 5, "cold before");
    check_int(cs, "(idle)", 7, "idle before");
    check_int(cs, "(wide 3)", 3, "wide before");

    const auto base = snap_of(cs);

    CHECK(rebind(cs, "cold", "(define (cold i) (+ i 1))", "same body"), "same-body rebind #t");
    check_eval(cs, "(eval-current)", "eval after same body");
    const auto same = snap_of(cs);
    CHECK(same.bridge == base.bridge && same.walk == base.walk && same.epoch == base.epoch &&
              same.relower == base.relower && same.body == base.body,
          std::format("same body keeps counters {}", snap_text(base, same)));
    check_int(cs, "(cold 4)", 5, "cold after same body");
    check_int(cs, "(hot 3)", 6, "hot after same body");
    check_int(cs, "(idle)", 7, "idle after same body");

    CHECK(rebind(cs, "cold", "(define (cold i) (+ i  1))", "whitespace"), "whitespace rebind #t");
    check_eval(cs, "(eval-current)", "eval after whitespace");
    const auto ws = snap_of(cs);
    CHECK(ws.bridge == same.bridge && ws.walk == same.walk && ws.epoch == same.epoch &&
              ws.relower == same.relower && ws.body == same.body,
          std::format("whitespace keeps counters {}", snap_text(same, ws)));

    CHECK(rebind(cs, "cold", "(define cold (lambda (i) (+ i 1)))", "lambda form"),
          "lambda-form rebind #t");
    check_eval(cs, "(eval-current)", "eval after lambda form");
    const auto lam = snap_of(cs);
    CHECK(lam.bridge == ws.bridge && lam.walk == ws.walk && lam.epoch == ws.epoch &&
              lam.relower == ws.relower && lam.body == ws.body,
          std::format("lambda form keeps counters {}", snap_text(ws, lam)));
    check_int(cs, "(cold 4)", 5, "cold after lambda form");

    CHECK(rebind(cs, "wide", wide_src(false), "same wide"), "same wide rebind #t");
    check_eval(cs, "(eval-current)", "eval after same wide");
    const auto wide_same = snap_of(cs);
    CHECK(wide_same.bridge == lam.bridge && wide_same.walk == lam.walk &&
              wide_same.epoch == lam.epoch && wide_same.relower == lam.relower &&
              wide_same.body == lam.body,
          std::format("large same body keeps counters {}", snap_text(lam, wide_same)));
    check_int(cs, "(wide 3)", 3, "wide after same body");
    check_int(cs, "(idle)", 7, "idle after wide same body");

    CHECK(rebind(cs, "cold", "(define (cold i) (+ i 2))", "plus two"), "real cold rebind #t");
    check_eval(cs, "(eval-current)", "eval after real cold");
    const auto real = snap_of(cs);
    // (hot 3) = (0+2) + (1+2) + (2+2)
    check_int(cs, "(cold 4)", 6, "cold sees +2");
    check_int(cs, "(hot 3)", 9, "hot sees the new cold");
    check_int(cs, "(idle)", 7, "idle after real cold");
    CHECK(real.bridge == wide_same.bridge && real.walk == wide_same.walk &&
              real.epoch == wide_same.epoch,
          std::format("real small rebind does not bump bridge {}", snap_text(wide_same, real)));

    CHECK(rebind(cs, "wide", wide_src(true), "wide plus"), "real wide rebind #t");
    check_eval(cs, "(eval-current)", "eval after real wide");
    const auto wide_real = snap_of(cs);
    check_int(cs, "(wide 3)", 4, "wide sees +1");
    check_int(cs, "(idle)", 7, "idle after real wide");
    check_int(cs, "(hot 3)", 9, "hot stays on the new cold");
    CHECK(wide_real.bridge == real.bridge && wide_real.walk == real.walk &&
              wide_real.epoch == real.epoch,
          std::format("real large rebind does not bump bridge {}", snap_text(real, wide_real)));
}

} // namespace

int main() {
    ac5147_same_body_keeps_bridge();
    return RUN_ALL_TESTS();
}
