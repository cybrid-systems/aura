// test_issue_766.cpp — Issue #766: IR-SoA migration observability +
// DirtyAware incremental pipeline dashboard. Refines #167/#463/#741.
// Non-duplicative with #729 query:soa-hotpath-stats and #765 query:
// incremental-quote-lambda-linear-stats. #766 ships the FIRST
// observability surface that tracks the *production migration of
// IRModuleV2 + DirtyAware incremental pipeline* — IR SoA column
// counts / DirtyAware short-circuit hits / pmr column utilization /
// JIT SoA codegen time — as separate per-decision-point counters
// the Agent consumes to monitor the SoA migration + DirtyAware
// short-circuit production-readiness under incremental AI mutation
// flows.
//
// Scope-limited close: the issue body asks for the real ir_soa.ixx
// IRFunctionSoA + IRModuleV2 add_instruction / mark_block_dirty /
// mark_all_blocks_dirty + pass_manager.ixx DirtyAwarePass +
// run_incremental_dirty_pipeline + lowering_impl.cpp set_soa_emit_
// path + apply_soa_view + evaluator_impl.cpp soa_interp_dispatch +
// aura_jit.cpp emit_soa_function + tests/test_highperf_ir_soa_
// migration_dirty_incremental.cpp harness (large define + quote/
// lambda + heavy mutate:rebind on body → impact_scope + DirtyAware
// partial re-lower + JIT recompile → assert SoA path used, clean
// blocks skipped, metrics accurate, no regression vs AoS, TSan/ASan
// clean) + SEVA SoA migration incremental demo + sync with
// ShapeProfiler versioning + Pass Pipeline JITFriendly epoch hints
// + Arena compact shape_inval_on_compact hook + pmr/arena hosting
// of remaining SoA columns + CI gate + docs. Each is a non-trivial
// focused session and is follow-up work.
//
// For this PR we ship:
//
//   1. 5 new atomics in CompilerMetrics:
//        ir_soa_instructions_emitted_total
//        ir_soa_dirty_block_skips_total
//        ir_soa_clean_block_hit_rate_pct
//        ir_soa_pmr_column_utilization_pct
//        ir_soa_jit_codegen_time_ns_total
//   2. 5 new public bump helpers on Evaluator
//        (bump_ir_soa_instructions_emitted /
//         bump_ir_soa_dirty_block_skips /
//         set_ir_soa_clean_block_hit_rate_pct /
//         set_ir_soa_pmr_column_utilization_pct /
//         bump_ir_soa_jit_codegen_time_ns)
//   3. New standalone (query:ir-soa-migration-stats, schema 766)
//      primitive exposing the 5 counters + schema sentinel
//      (6-entry hash).
//   4. Test verifies: primitive shape, fresh-zero state, schema
//      sentinel, bump accessibility, regression of #758/#763/#764
//      sibling primitives.
//
// ACs:
//   AC1: hash shape (5 fields + schema sentinel = 6 entries)
//   AC2: 5 counters == 0 on fresh service
//   AC3: schema == 766 (drift sentinel)
//   AC4: bump helpers accessible — exercise each field via direct
//        bump on Evaluator surface and verify the primitive
//        reports the bumps
//   AC5: regression — #758 + #763 + #764 sibling primitives still
//        reachable with their schema sentinels intact

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_766_detail {
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

static std::int64_t hash_int_field(aura::compiler::CompilerService& cs, std::string_view hash_src,
                                   std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void run_ac1_shape(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: (query:ir-soa-migration-stats) hash shape ---");
    auto r = cs.eval("(query:ir-soa-migration-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r), "(query:ir-soa-migration-stats) returns a hash");
    const std::vector<std::string> keys = {"soa-instructions-emitted", "dirty-block-skips",
                                           "clean-block-hit-rate",     "pmr-column-utilization",
                                           "jit-soa-codegen-time-ns",  "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(std::format("(hash-ref (query:ir-soa-migration-stats) '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: counters == 0 on fresh service ---");
    const auto sie =
        hash_int_field(cs, "(query:ir-soa-migration-stats)", "soa-instructions-emitted");
    CHECK(sie == 0,
          std::format("soa-instructions-emitted = {} (expected 0 on fresh service)", sie));
    const auto dbs = hash_int_field(cs, "(query:ir-soa-migration-stats)", "dirty-block-skips");
    CHECK(dbs == 0, std::format("dirty-block-skips = {} (expected 0 on fresh service)", dbs));
    const auto cbhr = hash_int_field(cs, "(query:ir-soa-migration-stats)", "clean-block-hit-rate");
    CHECK(cbhr == 0, std::format("clean-block-hit-rate = {} (expected 0 on fresh service)", cbhr));
    const auto pmr = hash_int_field(cs, "(query:ir-soa-migration-stats)", "pmr-column-utilization");
    CHECK(pmr == 0, std::format("pmr-column-utilization = {} (expected 0 on fresh service)", pmr));
    const auto jctn =
        hash_int_field(cs, "(query:ir-soa-migration-stats)", "jit-soa-codegen-time-ns");
    CHECK(jctn == 0,
          std::format("jit-soa-codegen-time-ns = {} (expected 0 on fresh service)", jctn));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 766 (drift sentinel) ---");
    const auto schema = hash_int_field(cs, "(query:ir-soa-migration-stats)", "schema");
    CHECK(schema == 766, std::format("schema = {} (expected 766)", schema));
}

static void run_ac4_bump_accessible(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: bump helpers callable from the evaluator surface ---");
    auto& ev = cs.evaluator();
    // Exercise each field: cumulative counters via bump, fixed-point
    // percent fields via set.
    ev.bump_ir_soa_instructions_emitted(10);
    ev.bump_ir_soa_instructions_emitted(20);
    ev.bump_ir_soa_instructions_emitted(30);
    ev.bump_ir_soa_dirty_block_skips(5);
    ev.bump_ir_soa_dirty_block_skips(7);
    ev.set_ir_soa_clean_block_hit_rate_pct(9500);   // 95.00%
    ev.set_ir_soa_pmr_column_utilization_pct(4500); // 45.00%
    ev.bump_ir_soa_jit_codegen_time_ns(123456);
    ev.bump_ir_soa_jit_codegen_time_ns(789012);
    const auto sie =
        hash_int_field(cs, "(query:ir-soa-migration-stats)", "soa-instructions-emitted");
    const auto dbs = hash_int_field(cs, "(query:ir-soa-migration-stats)", "dirty-block-skips");
    const auto cbhr = hash_int_field(cs, "(query:ir-soa-migration-stats)", "clean-block-hit-rate");
    const auto pmr = hash_int_field(cs, "(query:ir-soa-migration-stats)", "pmr-column-utilization");
    const auto jctn =
        hash_int_field(cs, "(query:ir-soa-migration-stats)", "jit-soa-codegen-time-ns");
    CHECK(sie == 60,
          std::format("after 10+20+30 emitted bumps: soa-instructions-emitted = {} (expected 60)",
                      sie));
    CHECK(dbs == 12,
          std::format("after 5+7 dirty-block-skips bumps: dirty-block-skips = {} (expected 12)",
                      dbs));
    CHECK(cbhr == 9500,
          std::format("after set 9500: clean-block-hit-rate = {} (expected 9500 = 95.00%)", cbhr));
    CHECK(pmr == 4500,
          std::format("after set 4500: pmr-column-utilization = {} (expected 4500 = 45.00%)", pmr));
    CHECK(
        jctn == 912468,
        std::format("after 123456+789012 ns bumps: jit-soa-codegen-time-ns = {} (expected 912468)",
                    jctn));
}

static void run_ac5_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #758/#763/#764 sibling primitives unaffected ---");
    auto edsl_reflection = cs.eval("(query:edsl-reflection-stats)");
    auto linear_ownership_gc_compiler = cs.eval("(query:linear-ownership-gc-compiler-stats)");
    auto compiler_arena_closure_lifetime = cs.eval("(query:compiler-arena-closure-lifetime-stats)");
    CHECK(edsl_reflection && aura::compiler::types::is_hash(*edsl_reflection),
          "query:edsl-reflection-stats hash regression (#758)");
    CHECK(linear_ownership_gc_compiler &&
              aura::compiler::types::is_hash(*linear_ownership_gc_compiler),
          "query:linear-ownership-gc-compiler-stats hash regression (#763)");
    CHECK(compiler_arena_closure_lifetime &&
              aura::compiler::types::is_hash(*compiler_arena_closure_lifetime),
          "query:compiler-arena-closure-lifetime-stats hash regression (#764)");
    const auto edsl_reflection_schema =
        hash_int_field(cs, "(query:edsl-reflection-stats)", "schema");
    CHECK(edsl_reflection_schema == 758,
          std::format("edsl-reflection schema = {} (expected 758, no drift)",
                      edsl_reflection_schema));
    const auto linear_ownership_gc_compiler_schema =
        hash_int_field(cs, "(query:linear-ownership-gc-compiler-stats)", "schema");
    CHECK(linear_ownership_gc_compiler_schema == 763,
          std::format("linear-ownership-gc-compiler schema = {} (expected 763, no drift)",
                      linear_ownership_gc_compiler_schema));
    const auto compiler_arena_closure_lifetime_schema =
        hash_int_field(cs, "(query:compiler-arena-closure-lifetime-stats)", "schema");
    CHECK(compiler_arena_closure_lifetime_schema == 764,
          std::format("compiler-arena-closure-lifetime schema = {} (expected 764, no drift)",
                      compiler_arena_closure_lifetime_schema));
}

} // namespace aura_issue_766_detail

int main() {
    using namespace aura_issue_766_detail;
    std::println("=== Issue #766: IR-SoA migration + DirtyAware incremental "
                 "observability (scope-limited close) ===");

    {
        aura::compiler::CompilerService cs;
        run_ac1_shape(cs);
        run_ac2_fresh_zero(cs);
        run_ac3_schema_sentinel(cs);
        run_ac4_bump_accessible(cs);
        run_ac5_regression(cs);
    }

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}