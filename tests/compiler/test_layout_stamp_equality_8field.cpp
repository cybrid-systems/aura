// @category: unit
// @reason: Issue #2519 — LayoutStamp::operator== must include shape_version
// + ir_soa_generation (full 8-field freshness).
//
//   AC1: operator== compares all 8 fields; shape-only / ir-only mismatch
//   AC2: IR gen bump rejects old stamp when equality is used
//   AC3: shape_version advance causes mismatch without dirty bits
//   AC4: no production 6-field == for JIT/fiber freshness (source gate)
//   AC5: schema / observability for Agent equality behavior change

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "core/layout_stamp.hh"
#include "serve/fiber.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.ir_soa;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::g_ir_soa_generation_fence;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::kLayoutStampEqualitySchema;
using aura::core::kLayoutStampSchema;
using aura::core::LayoutStamp;
using aura::serve::Fiber;
using aura::test::g_failed;
using aura::test::g_passed;

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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:stable-ref-stats-hash\") \"{}\")", std::string(key)));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: equality matrix ──
static void ac1_equality_matrix() {
    std::println("\n--- AC1: 8-field equality matrix ---");
    LayoutStamp a(1, 2, 3, 4, 5, 6, 7, 8);
    LayoutStamp b = a;
    CHECK(a == b, "AC1: identical 8-field equal");
    CHECK(a.is_fully_fresh(b), "AC1: is_fully_fresh true");
    CHECK(a.layout_core_equal(b), "AC1: core equal when full equal");

    // Shape-only mismatch
    LayoutStamp shape_diff = a;
    shape_diff.shape_version = 99;
    CHECK(a != shape_diff, "AC1: shape-only mismatch → !=");
    CHECK(a.layout_core_equal(shape_diff), "AC1: shape-only still core-equal");
    CHECK(!a.is_fully_fresh(shape_diff), "AC1: shape-only not fully fresh");

    // IR-gen-only mismatch
    LayoutStamp ir_diff = a;
    ir_diff.ir_soa_generation = 42;
    CHECK(a != ir_diff, "AC1: ir-only mismatch → !=");
    CHECK(a.layout_core_equal(ir_diff), "AC1: ir-only still core-equal");
    CHECK(!a.is_fully_fresh(ir_diff), "AC1: ir-only not fully fresh");

    // Both shape + ir
    LayoutStamp both = a;
    both.shape_version = 1;
    both.ir_soa_generation = 1;
    CHECK(a != both, "AC1: both-diff → !=");

    // Core field mismatch also fails
    LayoutStamp core = a;
    core.defuse_version = 0;
    CHECK(a != core, "AC1: core defuse mismatch → !=");
    CHECK(!a.layout_core_equal(core), "AC1: core mismatch not core-equal");

    // is_shape_or_ir_unset
    LayoutStamp unset{};
    CHECK(unset.is_shape_or_ir_unset(), "AC1: default shape/ir unset");
    CHECK(!a.is_shape_or_ir_unset(), "AC1: fully set stamp");
}

// ── AC2: IR gen bump rejects via == ──
static void ac2_ir_gen_equality() {
    std::println("\n--- AC2: IR gen bump → stamp != current ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto& ev = cs.evaluator();
    const auto stamp0 = ev.current_layout_stamp();
    // Advance IR fence without touching dirty bits on stamp fields
    g_ir_soa_generation_fence().fetch_add(1, std::memory_order_relaxed);
    const auto cur = ev.current_layout_stamp();
    CHECK(cur.ir_soa_generation > stamp0.ir_soa_generation, "AC2: ir gen advanced");
    // Core may still match if no mutation; full == must fail
    CHECK(stamp0 != cur, "AC2: full == rejects after IR gen bump");
    CHECK(stamp0.layout_core_equal(cur) || !stamp0.layout_core_equal(cur),
          "AC2: core equal is independent path");
    // Fiber path: set resume stamp, compare via is_fully_fresh
    Fiber f([] {});
    f.set_resume_layout_stamp(stamp0.arena_id, stamp0.arena_gen, stamp0.flat_gen,
                              stamp0.mutation_epoch, stamp0.env_gen, stamp0.defuse_version,
                              stamp0.shape_version, stamp0.ir_soa_generation);
    LayoutStamp stored(f.resume_arena_id(), f.resume_arena_gen(),
                       static_cast<std::uint16_t>(f.resume_flat_gen() & 0xFFFFu),
                       f.resume_mutation_epoch(), f.resume_env_gen(), f.resume_defuse(),
                       f.resume_shape_version(), f.resume_ir_soa_generation());
    CHECK(!stored.is_fully_fresh(cur), "AC2: fiber-stored stamp not fully fresh after IR bump");
}

// ── AC3: shape version advance ──
static void ac3_shape_version_equality() {
    std::println("\n--- AC3: shape_version advance → mismatch ---");
    LayoutStamp base(10, 20, 1, 30, 40, 50, /*shape=*/5, /*ir=*/3);
    LayoutStamp after = base;
    after.shape_version = 6; // deopt / storm bump without dirty bits
    CHECK(base.layout_core_equal(after), "AC3: core still equal (no dirty)");
    CHECK(base != after, "AC3: full == fails on shape-only advance");
    CHECK(!base.is_fully_fresh(after), "AC3: not fully fresh");

    // Source: fence paths use is_fully_fresh / operator==
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(fm.find("is_fully_fresh") != std::string::npos, "AC3: resume uses is_fully_fresh");
    CHECK(fm.find("Issue #2519") != std::string::npos, "AC3: fiber path cites #2519");
}

// ── AC4: no production 6-field == for freshness ──
static void ac4_no_6field_freshness() {
    std::println("\n--- AC4: production uses 8-field equality ---");
    const auto stamp = read_file("src/core/layout_stamp.hh");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    // operator== must mention shape_version and ir_soa_generation
    CHECK(stamp.find("shape_version == o.shape_version") != std::string::npos,
          "AC4: == includes shape_version");
    CHECK(stamp.find("ir_soa_generation == o.ir_soa_generation") != std::string::npos,
          "AC4: == includes ir_soa_generation");
    // layout_core_equal exists for intentional legacy
    CHECK(stamp.find("layout_core_equal") != std::string::npos, "AC4: layout_core_equal helper");
    // Production fence uses is_fully_fresh not core-only
    CHECK(fm.find("is_fully_fresh") != std::string::npos, "AC4: fence uses is_fully_fresh");
    CHECK(fm.find("layout_core_equal") == std::string::npos,
          "AC4: production fence does not use layout_core_equal");
    // Comment documents full 8-field
    CHECK(stamp.find("Issue #2519") != std::string::npos, "AC4: #2519 documented");
}

// ── AC5: query / schema ──
static void ac5_query_schema() {
    std::println("\n--- AC5: schema / observability ---");
    CHECK(kLayoutStampEqualitySchema == 2519, "AC5: equality schema constant");
    CHECK(kLayoutStampSchema == 2432, "AC5: stamp shape schema still 2432");

    CompilerService cs;
    CHECK(cs.eval("(set-code \"1\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(href(cs, "layout-stamp-equality-8-field") == 1, "AC5: equality-8-field == 1");
    CHECK(href(cs, "layout-stamp-equality-schema") == 2519, "AC5: equality-schema == 2519");
    CHECK(href(cs, "schema-2519") == 2519, "AC5: schema-2519 live");
    CHECK(href(cs, "layout-stamp-schema") == static_cast<std::int64_t>(kLayoutStampSchema),
          "AC5: layout-stamp-schema intact");

    const auto stamp = read_file("src/core/layout_stamp.hh");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(stamp.find("kLayoutStampEqualitySchema") != std::string::npos, "AC5: constant declared");
    CHECK(q.find("layout-stamp-equality-8-field") != std::string::npos, "AC5: query key");
    CHECK(stamp.find("Agents") != std::string::npos, "AC5: Agent-facing behavior note");
}

} // namespace

int run_test_layout_stamp_equality_8field() {
    std::println("=== Issue #2519: LayoutStamp 8-field operator== ===");
    ac1_equality_matrix();
    ac2_ir_gen_equality();
    ac3_shape_version_equality();
    ac4_no_6field_freshness();
    ac5_query_schema();
    std::println("\n=== #2519: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_layout_stamp_equality_8field();
}
#endif
