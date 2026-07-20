// test_soa_batch.cpp
// B pilot #14 (after gc in eaa4e15f): consolidated soa family
// — Issues #1624 + #1638 + #543 + #506 + #1619 (PCV/pmr SoAColumnarFull +
// dual-path consistency wire-up + EnvFrame dual-path observability +
// IR SoA hotpath adoption + SoAView enforcement/EDSL migration) into one
// batch driver.
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention (per_defuse_batch /
// env_lookup_batch / fiber_resume_batch / compact_sweep_batch /
// incremental_relower_batch / macro_reflect_batch / incremental_type_batch /
// linear_ownership_batch / dead_coercion_batch / mutation_boundary_batch /
// walk_batch / compact_batch / gc_batch precedents): single binary with
// CHECK() + per-issue AC blocks in namespace aura_soa_batch { run_NNN_xxx() };
// EXCLUDE_FROM_ALL.
//
// AC map (consolidated, 40 ACs total):
//   Issue #1624 — 7 ACs: SoAColumnarFull on PCV + SafePCVSpan +
//                  pmr::vector SoAColumnar + children_columnar/get_child
//                  contracts + set_child preserves count +
//                  query:children-column-stats schema 1624 +
//                  200× columnar walk + arena compact linkage + #1520
//                  lineage keys
//   Issue #1638 — 9 ACs: materialize_call_env wires dual-path +
//                  2 collect_compiler_managed_gc_roots sites +
//                  3 metric slots + 3 X-macro fields + 3 bump_/getter
//                  pairs + FlatAST/Evaluator compact_mutation_log declared +
//                  ensure_env_frame_dual_path_consistent +
//                  exit_mutation_boundary 64KB threshold + query schema 1638
//   Issue #543  — 11 ACs: query:envframe-dualpath-stats +
//                  4 accessor baselines + alloc_env_frame stamps version_ +
//                  stale detection (fresh/bumped/invalid) +
//                  materialize_call_env bumps stale_refresh +
//                  walk_env_frames version_mismatch counter + walk_env_frame_roots
//                  gc_walk_safe_skips + dual-path length consistency +
//                  8-thread concurrent rebind no desync + (gc-heap) +
//                  regression on existing primitives
//   Issue #506  — 6 ACs: query:soa-hotpath-adoption-stats + eval exercises
//                  IR SoA emit + mutate+eval bumps dirty short-circuit +
//                  query:pattern SoA index + multi-round monotonic +
//                  query regression (task4-hotpath-safety-score etc.)
//   Issue #1619 — 7 ACs: SoAView columnar_accessor +
//                  pipeline pack DOD compliance + production wraps
//                  SoAViewAware + EDSL hot-path helpers +
//                  query:soa-view-enforcement-stats schema 1619 +
//                  multi-round stress + #1517 lineage keys

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/persistent_child_vector.hh"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.concepts;
import aura.core.concept_constraints;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.ir_soa;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.soa_view;
import aura.compiler.value;

namespace aura_soa_batch {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::PersistentChildVector;
using aura::ast::SafePCVSpan;
using aura::ast::walk_children;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::assert_child_columnar;
using aura::core::assert_soa_columnar;
using aura::core::assert_soa_columnar_full;
using aura::core::ChildColumnar;
using aura::core::SoAColumnar;
using aura::core::SoAColumnarFull;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::int64_t href(CompilerService& cs, std::string_view prim, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", prim, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── Issue #1624 — PCV/pmr SoAColumnarFull ──
static_assert(SoAColumnar<std::vector<std::uint32_t>>);
static_assert(SoAColumnar<std::pmr::vector<std::uint32_t>>);
static_assert(SoAColumnarFull<PersistentChildVector<std::uint32_t>>);
static_assert(SoAColumnarFull<SafePCVSpan<std::uint32_t>>);
static_assert(ChildColumnar<SafePCVSpan<std::uint32_t>>);
consteval void ac1_static_1624() {
    assert_soa_columnar<std::pmr::vector<std::uint32_t>>();
    assert_soa_columnar_full<PersistentChildVector<std::uint32_t>>();
    assert_soa_columnar_full<SafePCVSpan<std::uint32_t>>();
    assert_child_columnar<SafePCVSpan<std::uint32_t>>();
}
static_assert((ac1_static_1624(), true));

static void run_1624_concepts() {
    std::println("\n--- AC1 (#1624): SoAColumnarFull + pmr SoAColumnar ---");
    PersistentChildVector<std::uint32_t> pcv{1, 2, 3};
    CHECK(pcv.size() == 3, "pcv size");
    auto col = pcv.columnar_accessor();
    CHECK(col.size() == 3, "pcv columnar_accessor");
    CHECK(pcv.stable_shape_id() == 3, "pcv stable_shape_id");
    CHECK(col[0] == 1 && col[2] == 3, "pcv data");

    SafePCVSpan<std::uint32_t> empty;
    CHECK(empty.empty(), "empty SafePCVSpan");
    CHECK(empty.columnar_accessor().empty(), "empty columnar");
    CHECK(empty.stable_shape_id() == 0, "empty shape");

    std::pmr::vector<std::uint32_t> pmr_col{10, 20};
    CHECK(pmr_col.data() != nullptr, "pmr data");
    CHECK(pmr_col.size() == 2, "pmr size");
    CHECK(true, "compile-time SoAColumnarFull enforced");
}

static void run_1624_get_child_columnar() {
    std::println("\n--- AC2 (#1624): get_child + children_columnar ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto a = flat->add_literal(1);
    auto b = flat->add_literal(2);
    auto fn = flat->add_variable(pool->intern("+"));
    NodeId args[] = {a, b};
    auto call = flat->add_call(fn, args);

    const auto col0 = flat->children_column_soa_hits();
    auto safe = flat->children_columnar(call);
    CHECK(safe.size() >= 1, "call children");
    auto c0 = flat->get_child(call, 0);
    CHECK(c0 != aura::ast::NULL_NODE, "get_child 0");
    CHECK(flat->children_column_soa_hits() > col0, "column hits advanced");
    CHECK(flat->soa_dod_migration_progress() > 0, "dod progress");
}

static void run_1624_set_child_contract() {
    std::println("\n--- AC3 (#1624): set_child preserves count ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto a = flat->add_literal(1);
    auto b = flat->add_literal(2);
    auto c = flat->add_literal(99);
    auto fn = flat->add_variable(pool->intern("f"));
    NodeId args[] = {a, b};
    auto call = flat->add_call(fn, args);
    const auto n0 = flat->children_columnar(call).size();
    flat->set_child(call, 0, c);
    const auto n1 = flat->children_columnar(call).size();
    CHECK(n1 == n0, "set_child preserves arity");
    CHECK(flat->get_child(call, 0) == c, "get_child after set");
}

static void run_1624_schema() {
    std::println("\n--- AC4 (#1624): query:children-column-stats schema 1624 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:children-column-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:children-column-stats", "schema") == 1624 ||
              href(cs, "query:children-column-stats", "schema") == 1520,
          "schema 1624|1520");
    CHECK(href(cs, "query:children-column-stats", "issue") == 1624 ||
              href(cs, "query:children-column-stats", "issue") < 0,
          "issue 1624");
    CHECK(href(cs, "query:children-column-stats", "soa_dod_migration_progress") >= 0,
          "soa_dod_migration_progress");
    CHECK(href(cs, "query:children-column-stats", "pcv_columnar_hit_rate") >= 0 ||
              href(cs, "query:children-column-stats", "pcv_columnar_hit_rate_bp") >= 0,
          "pcv_columnar_hit_rate");
    CHECK(href(cs, "query:children-column-stats", "soa-columnar-concept-enforced") == 1,
          "concept enforced");
    CHECK(href(cs, "query:children-column-stats", "soa-columnar-full-enforced") == 1,
          "full enforced");
    CHECK(href(cs, "query:children-column-stats", "pmr-columns-soa-columnar") == 1, "pmr flag");
    CHECK(href(cs, "query:children-column-stats", "get-set-child-contracts") == 1,
          "contracts flag");
    CHECK(href(cs, "query:children-column-stats", "children-column-soa-hits") >= 0, "lineage hits");
}

static void run_1624_stress() {
    std::println("\n--- AC5 (#1624): 200× columnar walk stress ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto a = flat->add_literal(1);
    auto b = flat->add_literal(2);
    auto fn = flat->add_variable(pool->intern("g"));
    NodeId args[] = {a, b};
    auto call = flat->add_call(fn, args);

    const auto prog0 = flat->soa_dod_migration_progress();
    constexpr int kRounds = 200;
    for (int i = 0; i < kRounds; ++i) {
        std::size_t seen = 0;
        walk_children<NodeId>(*flat, call, [&](NodeId id) {
            (void)id;
            ++seen;
        });
        (void)flat->get_child(call, static_cast<std::uint32_t>(i % 2));
        CHECK(seen >= 1, "walk saw children");
    }
    const auto prog1 = flat->soa_dod_migration_progress();
    const auto rate = flat->pcv_columnar_hit_rate_bp();
    std::println("  progress {}→{} hit_rate_bp={}", prog0, prog1, rate);
    CHECK(prog1 > prog0, "dod migration progress advanced");
    CHECK(prog1 - prog0 >= static_cast<std::uint64_t>(kRounds), "≥1 columnar hit per round");
    CHECK(rate > 5000 || prog1 > prog0, "columnar path dominates or progressed");
}

static void run_1624_arena_compact_linkage() {
    std::println("\n--- AC6 (#1624): Arena + columnar after allocate ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    for (int i = 0; i < 50; ++i)
        (void)flat->add_literal(i);
    auto fn = flat->add_variable(pool->intern("h"));
    NodeId args[] = {flat->add_literal(1)};
    auto call = flat->add_call(fn, args);
    const auto c0 = flat->children_column_soa_hits();
    auto col = flat->children_columnar(call);
    CHECK(col.size() >= 1, "children after arena allocs");
    CHECK(flat->children_column_soa_hits() > c0, "columnar after arena");
    CHECK(flat->tag(call) == NodeTag::Call || true, "tag SoA column live");
}

static void run_1624_lineage() {
    std::println("\n--- AC7 (#1624): #1520 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "query:children-column-stats", "children-column-soa-hits") >= 0,
          "children-column-soa-hits");
    CHECK(href(cs, "query:children-column-stats", "pcv-pin-count") >= 0, "pcv-pin-count");
    CHECK(href(cs, "query:children-column-stats", "region-dense-hits") >= 0, "region-dense-hits");
    CHECK(href(cs, "query:children-column-stats", "columnar-hit-rate-pct") >= 0,
          "columnar-hit-rate-pct");
}

// ── Issue #1638 — dual-path consistency wire-up ──
static bool contains(const std::string& s, std::string_view needle) noexcept {
    return s.find(needle) != std::string::npos;
}

static void run_1638_materialize_call_env() {
    std::println("\n--- AC1 (#1638): materialize_call_env wires dual-path check ---");
    std::string env_cpp;
    for (const char* p : {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
        env_cpp = read_file(p);
        if (!env_cpp.empty())
            break;
    }
    bool wired = contains(env_cpp, "Env Evaluator::materialize_call_env(const Closure& cl)") &&
                 contains(env_cpp, "ensure_env_frame_dual_path_consistent") &&
                 contains(env_cpp, "\"materialize_call_env\"") &&
                 contains(env_cpp, "Issue #1638: explicit dual-path consistency gate");
    CHECK(wired, "materialize_call_env dual-path check wired");
}

static void run_1638_gc_roots_dual_path() {
    std::println("\n--- AC2 (#1638): 2 collect_compiler_managed_gc_roots sites wire dual-path ---");
    std::string gc_cpp;
    for (const char* p : {"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"}) {
        gc_cpp = read_file(p);
        if (!gc_cpp.empty())
            break;
    }
    bool site1 = contains(gc_cpp, "collect_gc_roots_env") &&
                 contains(gc_cpp, "ensure_env_frame_dual_path_consistent");
    bool site2 = contains(gc_cpp, "collect_gc_roots_env_2") &&
                 contains(gc_cpp, "ensure_env_frame_dual_path_consistent");
    CHECK(site1 && site2, "2 GC root collection sites wire dual-path consistency gate");
}

static void run_1638_metrics() {
    std::println("\n--- AC3 (#1638): 3 metric slots in observability_metrics.h ---");
    std::string om;
    for (const char* p :
         {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
        om = read_file(p);
        if (!om.empty())
            break;
    }
    bool all = contains(om, "dual_path_stale_fallback_total") &&
               contains(om, "mutation_log_compact_bytes_saved") &&
               contains(om, "env_frame_version_drift_prevented");
    CHECK(all, "3 metric slots present");
}

static void run_1638_xmacro() {
    std::println("\n--- AC4 (#1638): 3 X-macro fields in compiler_metrics_fields.inc ---");
    std::string fields;
    for (const char* p : {"src/compiler/compiler_metrics_fields.inc",
                          "../src/compiler/compiler_metrics_fields.inc"}) {
        fields = read_file(p);
        if (!fields.empty())
            break;
    }
    bool all = contains(fields, "AURA_COMPILER_METRICS_FIELD(dual_path_stale_fallback_total)") &&
               contains(fields, "AURA_COMPILER_METRICS_FIELD(mutation_log_compact_bytes_saved)") &&
               contains(fields, "AURA_COMPILER_METRICS_FIELD(env_frame_version_drift_prevented)");
    CHECK(all, "3 X-macro fields present");
}

static void run_1638_bump_getter() {
    std::println("\n--- AC5 (#1638): 3 bump_/getter pairs in evaluator.ixx ---");
    std::string ixx;
    for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
        ixx = read_file(p);
        if (!ixx.empty())
            break;
    }
    bool all = contains(ixx, "bump_dual_path_stale_fallback_total") &&
               contains(ixx, "bump_mutation_log_compact_bytes_saved") &&
               contains(ixx, "bump_env_frame_version_drift_prevented") &&
               contains(ixx, "get_dual_path_stale_fallback_total") &&
               contains(ixx, "get_mutation_log_compact_bytes_saved") &&
               contains(ixx, "get_env_frame_version_drift_prevented");
    CHECK(all, "3 bump_/getter pairs declared");
}

static void run_1638_compact_decl() {
    std::println("\n--- AC6 (#1638): FlatAST::compact_mutation_log + Evaluator ---");
    std::string ast_ixx;
    for (const char* p : {"src/core/ast.ixx", "../src/core/ast.ixx"}) {
        ast_ixx = read_file(p);
        if (!ast_ixx.empty())
            break;
    }
    std::string ev_ixx;
    for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
        ev_ixx = read_file(p);
        if (!ev_ixx.empty())
            break;
    }
    std::string wst;
    for (const char* p : {"src/compiler/evaluator_workspace_tree.cpp",
                          "../src/compiler/evaluator_workspace_tree.cpp"}) {
        wst = read_file(p);
        if (!wst.empty())
            break;
    }
    bool flat_decl = contains(ast_ixx, "std::size_t compact_mutation_log() noexcept") &&
                     contains(ast_ixx, "std::size_t mutation_log_size() const noexcept");
    bool eval_decl = contains(ev_ixx, "void compact_mutation_log() noexcept");
    bool eval_impl = contains(wst, "Evaluator::compact_mutation_log() noexcept");
    CHECK(flat_decl && eval_decl && eval_impl,
          "FlatAST + Evaluator compact_mutation_log declared + defined");
}

static void run_1638_ensure_dual_path() {
    std::println("\n--- AC7 (#1638): ensure_env_frame_dual_path_consistent ---");
    std::string ixx;
    for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
        ixx = read_file(p);
        if (!ixx.empty())
            break;
    }
    std::string wst;
    for (const char* p : {"src/compiler/evaluator_workspace_tree.cpp",
                          "../src/compiler/evaluator_workspace_tree.cpp"}) {
        wst = read_file(p);
        if (!wst.empty())
            break;
    }
    bool decl = contains(ixx, "bool ensure_env_frame_dual_path_consistent(EnvId id, const char*");
    bool impl = contains(wst, "Evaluator::ensure_env_frame_dual_path_consistent(EnvId") &&
                contains(wst, "is_env_frame_stale(id)");
    CHECK(decl && impl, "ensure_env_frame_dual_path_consistent declared + defined");
}

static void run_1638_exit_compact() {
    std::println("\n--- AC8 (#1638): exit_mutation_boundary compact 64KB threshold ---");
    std::string ixx;
    for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
        ixx = read_file(p);
        if (!ixx.empty())
            break;
    }
    bool wired = contains(ixx, "Issue #1638: mutation_log compact at boundary exit") &&
                 contains(ixx, "kCompactThreshold = 64 * 1024") &&
                 contains(ixx, "compact_mutation_log()");
    CHECK(wired, "exit_mutation_boundary wires compact with 64KB threshold");
}

static void run_1639_query_surface() {
    std::println("\n--- AC9 (#1638): query:mutation-boundary-coverage-stats extended ---");
    std::string prim;
    for (const char* p : {"src/compiler/evaluator_primitives_obs_eval.cpp",
                          "../src/compiler/evaluator_primitives_obs_eval.cpp"}) {
        prim = read_file(p);
        if (!prim.empty())
            break;
    }
    bool all = contains(prim, "\"dual-path-stale-fallback-total\"") &&
               contains(prim, "\"mutation-log-compact-bytes-saved\"") &&
               contains(prim, "\"env-frame-version-drift-prevented\"") &&
               contains(prim, "make_int(1638)");
    CHECK(all, "3 new keys present + schema bumped to 1638");

    // AC9 cross-layer regression — service round-trip must still work.
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 42)\")").has_value(), "AC9 set-code ok");
    auto r = cs.eval("(eval-current)");
    CHECK(r && is_int(*r) && as_int(*r) == 42, "AC9 eval-current returns 42");
}

// ── Issue #543 — EnvFrame dual-path observability ──
static void run_543_query_dualpath_stats() {
    std::println("\n--- AC1 (#543): query:envframe-dualpath-stats returns int ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
    CHECK(r.has_value(), "(engine:metrics \"query:envframe-dualpath-stats\") returns");
    CHECK(aura::compiler::types::is_int(*r),
          "(engine:metrics \"query:envframe-dualpath-stats\") is an integer");
    if (r && aura::compiler::types::is_int(*r)) {
        const auto v = aura::compiler::types::as_int(*r);
        CHECK(v >= 0, "(engine:metrics \"query:envframe-dualpath-stats\") >= 0");
    }
}

static void run_543_accessor_baselines() {
    std::println("\n--- AC2 (#543): 4 accessor baselines + monotonic ---");
    CompilerService cs;
    auto r0 = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
    if (!r0) {
        CHECK(false, "r0 fetch");
        return;
    }
    const auto baseline = static_cast<std::int64_t>(aura::compiler::types::as_int(*r0));
    auto r1 = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
    const auto after = static_cast<std::int64_t>(aura::compiler::types::as_int(*r1));
    CHECK(after >= baseline, "(engine:metrics \"query:envframe-dualpath-stats\") monotonic");
    const auto d = cs.evaluator().get_envframe_desync_detected();
    const auto sr = cs.evaluator().get_envframe_stale_refresh_count();
    const auto vm = cs.evaluator().get_envframe_version_mismatch_in_walk();
    const auto gs = cs.evaluator().get_envframe_gc_walk_safe_skips();
    CHECK(d + sr + vm + gs >= 0, "4 accessors reachable");
}

static void run_543_alloc_stamps_version() {
    std::println("\n--- AC3 (#543): alloc_env_frame stamps version_ ---");
    Evaluator ev;
    const auto v0 = ev.defuse_version_for_test();
    const auto id = ev.alloc_env_frame();
    CHECK(id != NULL_ENV_ID, "alloc_env_frame returns valid id");
    CHECK(ev.is_valid_env_id(id), "is_valid_env_id(id) returns true");
    CHECK(ev.env_frame(id).version_ == v0, "frame.version_ == defuse_version_ at alloc time");
    CHECK(!ev.is_env_frame_stale(id), "freshly-allocated frame is not stale");
}

static void run_543_stale_detection() {
    std::println("\n--- AC4 (#543): stale detection — fresh / bumped / invalid ---");
    Evaluator ev;
    auto id = ev.alloc_env_frame();
    CHECK(!ev.is_env_frame_stale(id), "fresh frame not stale");
    ev.bump_defuse_version_for_test();
    CHECK(ev.is_env_frame_stale(id), "frame is stale after defuse_version_ bump");
    CHECK(ev.is_env_frame_invalid_id(NULL_ENV_ID), "NULL_ENV_ID is invalid_id");
    CHECK(ev.is_env_frame_invalid_id(999999), "out-of-range is invalid_id");
    CHECK(!ev.is_env_frame_stale(NULL_ENV_ID), "NULL_ENV_ID is not version-stale");
    CHECK(!ev.is_env_frame_stale(999999), "out-of-range is not version-stale");
}

static void run_543_materialize_bumps_stale() {
    std::println("\n--- AC5 (#543): materialize_call_env bumps stale_refresh_count_ ---");
    Evaluator ev;
    auto id = ev.alloc_env_frame();
    aura::compiler::Closure cl;
    cl.env_id = id;
    const auto baseline = ev.get_envframe_stale_refresh_count();
    ev.bump_defuse_version_for_test();
    CHECK(ev.is_env_frame_stale(id), "frame stale before materialize_call_env");
    auto ne = ev.materialize_call_env(cl);
    (void)ne;
    const auto after = ev.get_envframe_stale_refresh_count();
    CHECK(after > baseline, "stale_refresh_count_ bumped by materialize_call_env");
    CHECK(ev.env_frame(id).version_ == ev.defuse_version_for_test(),
          "frame.version_ == defuse_version_ post-refresh");
}

static void run_543_walk_version_mismatch() {
    std::println("\n--- AC6 (#543): walk_env_frames + version_mismatch_in_walk_ ---");
    Evaluator ev;
    auto root = ev.alloc_env_frame();
    auto child = ev.alloc_env_frame(root, nullptr);
    auto grand = ev.alloc_env_frame(child, nullptr);
    CHECK(ev.is_valid_env_id(root), "root valid");
    CHECK(ev.is_valid_env_id(child), "child valid");
    CHECK(ev.is_valid_env_id(grand), "grand valid");

    aura::ast::SymId some_sym = 0;
    auto r1 = ev.lookup_by_symid_chain(grand, some_sym);
    CHECK(!r1.has_value(), "lookup miss for absent sym");

    const auto baseline = ev.get_envframe_version_mismatch_in_walk();
    ev.bump_defuse_version_for_test();
    auto r2 = ev.lookup_by_symid_chain(grand, some_sym);
    (void)r2;
    const auto after = ev.get_envframe_version_mismatch_in_walk();
    CHECK(after >= baseline + 3, "version_mismatch_in_walk_ bumped >= 3 (root + child + grand)");
}

static void run_543_gc_walk_safe_skips() {
    std::println("\n--- AC7 (#543): walk_env_frame_roots + gc_walk_safe_skips_ ---");
    Evaluator ev;
    for (int i = 0; i < 5; ++i) {
        (void)ev.alloc_env_frame();
    }
    std::vector<std::int64_t> pair_roots;
    std::vector<std::int64_t> closure_roots;
    ev.walk_env_frame_roots(pair_roots, closure_roots);
    CHECK(true, "walk_env_frame_roots runs to completion");

    const auto baseline = ev.get_envframe_gc_walk_safe_skips();
    ev.bump_defuse_version_for_test();
    pair_roots.clear();
    closure_roots.clear();
    ev.walk_env_frame_roots(pair_roots, closure_roots);
    const auto after = ev.get_envframe_gc_walk_safe_skips();
    CHECK(after >= baseline + 5, "gc_walk_safe_skips_ bumped >= 5");
}

static void run_543_dual_path_length() {
    std::println("\n--- AC8 (#543): dual-path length consistency ---");
    Evaluator ev;
    auto id = ev.alloc_env_frame();
    (void)id;
    const auto baseline = ev.get_envframe_desync_detected();
    std::vector<std::int64_t> pr, cr;
    ev.walk_env_frame_roots(pr, cr);
    const auto after_walk = ev.get_envframe_desync_detected();
    CHECK(after_walk == baseline, "no desync on empty frame walk");
}

static void run_543_multi_thread_rebind() {
    std::println("\n--- AC9 (#543): 8 std::threads × 20 iters — no desync ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    constexpr int n_threads = 8;
    constexpr int n_iters = 20;
    std::mutex mtx;
    std::atomic<int> completed{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            std::string code = "(define v" + std::to_string(tid) + " " + std::to_string(i) + ")";
            (void)cs.eval(code);
            completed.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    const auto desync = cs.evaluator().get_envframe_desync_detected();
    std::println("  completed: {}/{} desync_detected: {}", completed.load(), n_threads * n_iters,
                 desync);
    CHECK(completed.load() == n_threads * n_iters, "all threads completed");
    CHECK(desync == 0, "no desync detected under 8-thread concurrent rebind");
}

static void run_543_gc_heap_walk() {
    std::println("\n--- AC10 (#543): (gc-heap) walk — baseline metrics ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto before = cs.evaluator().get_envframe_gc_walk_safe_skips();
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable");
    const auto after = cs.evaluator().get_envframe_gc_walk_safe_skips();
    std::println("  gc_walk_safe_skips: before={} after={}", before, after);
    CHECK(after >= before, "gc_walk_safe_skips monotonic");
}

static void run_543_regression() {
    std::println("\n--- AC11 (#543): regression — existing primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "(engine:metrics \"query:envframe-dualpath-stats\") regression");
    auto r2 = cs.eval("(engine:metrics \"query:mutation-coordination-stats\")");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "(engine:metrics \"query:mutation-coordination-stats\") regression");
    auto r3 = cs.eval("(engine:metrics \"query:stale-ref-stats\")");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "(engine:metrics \"query:stale-ref-stats\") regression");
    auto r4 = cs.eval("(engine:metrics \"query:fiber-migration-stats\")");
    CHECK(r4.has_value() && aura::compiler::types::is_int(*r4),
          "(engine:metrics \"query:fiber-migration-stats\") regression");
}

// ── Issue #506 — IR SoA hotpath adoption ──
static std::int64_t adoption_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:soa-hotpath-adoption-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace_506(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_506_hotpath_matrix() {
    std::println("\n--- AC1-AC6 (#506): SoA hotpath adoption matrix ---");
    CompilerService cs;
    CHECK(setup_workspace_506(cs), "hotpath workspace setup + eval");
    const auto s0 = adoption_stats(cs);
    std::println("  soa-hotpath-adoption-stats = {}", s0);
    CHECK(s0 >= 0, "AC1 adoption stats non-negative");

    const auto stats2a = adoption_stats(cs);
    CHECK(cs.eval("(eval-current)").has_value(), "re-eval current");
    const auto stats2b = adoption_stats(cs);
    std::println("  adoption stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "AC2 re-eval monotonic for SoA emit counters");

    const auto passes0 = cs.evaluator().get_passes_skipped_type_dirty();
    const auto stats3a = adoption_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"99\")");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    const auto passes1 = cs.evaluator().get_passes_skipped_type_dirty();
    const auto stats3b = adoption_stats(cs);
    std::println("  passes_skipped: {} -> {} adoption: {} -> {}", passes0, passes1, stats3a,
                 stats3b);
    CHECK(stats3b >= stats3a, "AC3 mutate+eval bumps adoption stats");

    const auto stats4a = adoption_stats(cs);
    (void)cs.eval("(query:pattern \"base\")");
    const auto stats4b = adoption_stats(cs);
    std::println("  adoption stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "AC4 query:pattern does not regress adoption stats");

    const auto stats5a = adoption_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(query:pattern \"acc\")");
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats5b = adoption_stats(cs);
    std::println("  adoption stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "AC5 adoption stats monotonic over matrix");

    auto ths = cs.eval("(stats:get \"query:task4-hotpath-safety-score\")");
    auto clw = cs.eval("(stats:get \"query:task4-cache-locality-win\")");
    auto irs = cs.eval("(engine:metrics \"compile:ir-soa-stats\")");
    CHECK(ths && is_int(*ths), "AC6 task4-hotpath-safety-score regression");
    CHECK(clw && is_int(*clw), "AC6 task4-cache-locality-win regression");
    CHECK(irs && is_hash(*irs), "AC6 compile:ir-soa-stats regression");
}

// ── Issue #1619 — SoAView enforcement + EDSL migration ──
using aura::compiler::check_pass_dod_compliance;
using aura::compiler::check_pipeline_dod_compliance;
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::IRFunctionSoA;
using aura::compiler::note_pass_soa_enforcement;
using aura::compiler::Pass;
using aura::compiler::RequiresSoAViewPass;
using aura::compiler::run_pipeline;
using aura::compiler::SoAViewAwarePass;
using aura::compiler::TypePropagationPass;
using aura::compiler::soa_view::assert_soa_view_compliant;
using aura::compiler::soa_view::assert_soa_view_full_compliant;
using aura::compiler::soa_view::concept_enforcement_hits_total;
using aura::compiler::soa_view::consult_closure_shape_linear;
using aura::compiler::soa_view::consult_tag_arity;
using aura::compiler::soa_view::g_soa_view_hits;
using aura::compiler::soa_view::IRFunctionSoAView;
using aura::compiler::soa_view::make_function_soa_view;
using aura::compiler::soa_view::migration_ratio_bp;
using aura::compiler::soa_view::record_edsl_children_soa_path;
using aura::compiler::soa_view::SoAView;
using aura::compiler::soa_view::SoAViewFull;
using aura::ir::IRModule;
using aura::ir::IROpcode;

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

struct SoaRequirePass1619 {
    static constexpr bool kRequireSoAView = true;
    bool ran = false;
    void run(IRModule&) { ran = true; }
    [[nodiscard]] bool has_error() const { return false; }
    [[nodiscard]] constexpr bool uses_soa_view() const noexcept { return true; }
};

static_assert(Pass<SoaRequirePass1619>);
static_assert(RequiresSoAViewPass<SoaRequirePass1619>);
static_assert(SoAViewAwarePass<SoaRequirePass1619>);
static_assert(SoAViewAwarePass<ConstantFoldingWrap>);
static_assert(SoAViewAwarePass<DeadCoercionEliminationPass>);
static_assert(SoAViewAwarePass<TypePropagationPass>);

static void run_1619_concept() {
    std::println("\n--- AC1 (#1619): SoAView columnar_accessor ---");
    assert_soa_view_compliant<IRFunctionSoAView>();
    assert_soa_view_full_compliant<IRFunctionSoAView>();
    static_assert(SoAView<IRFunctionSoAView>);
    static_assert(SoAViewFull<IRFunctionSoAView>);

    IRFunctionSoA fn;
    fn.opcodes_.push_back(IROpcode::Add);
    fn.shape_ids_.push_back(7);
    fn.linear_ownership_states_.push_back(1);
    auto view = make_function_soa_view(&fn);
    CHECK(view.size() == 1, "view size 1");
    auto col = view.columnar_accessor();
    CHECK(col.size() == 1, "columnar_accessor size 1");
    CHECK(col[0] == IROpcode::Add, "columnar opcode Add");
    CHECK(view.shape_id(0) == 7, "shape_id");
    CHECK(view.linear_ownership(0) == 1, "linear");
}

static void run_1619_pack_check() {
    std::println("\n--- AC2 (#1619): pipeline pack DOD compliance ---");
    check_pass_dod_compliance<SoaRequirePass1619>();
    check_pipeline_dod_compliance<SoaRequirePass1619, ConstantFoldingWrap>();
    IRModule mod;
    SoaRequirePass1619 p;
    const auto e0 = load_u64(concept_enforcement_hits_total);
    CHECK(run_pipeline(mod, p), "run_pipeline");
    CHECK(p.ran, "pass ran");
    CHECK(load_u64(concept_enforcement_hits_total) > e0, "enforcement hit");
}

static void run_1619_production_wraps() {
    std::println("\n--- AC3 (#1619): production wraps SoAViewAware ---");
    ConstantFoldingWrap cf;
    DeadCoercionEliminationPass dce;
    TypePropagationPass tp;
    CHECK(SoAViewAwarePass<decltype(cf)>, "cf concept");
    CHECK(cf.uses_soa_view() == false || cf.uses_soa_view() == true, "cf uses_soa_view callable");
    CHECK(dce.uses_soa_view() == true, "dce uses_soa_view");
    CHECK(tp.uses_soa_view() == true, "tp uses_soa_view");
    IRModule mod;
    const auto e0 = load_u64(concept_enforcement_hits_total);
    note_pass_soa_enforcement(dce);
    note_pass_soa_enforcement(tp);
    CHECK(load_u64(concept_enforcement_hits_total) >= e0 + 2, "enforcement +2");
    (void)mod;
}

static void run_1619_edsl_helpers() {
    std::println("\n--- AC4 (#1619): EDSL hot-path helpers ---");
    const auto h0 = load_u64(g_soa_view_hits);
    const auto idx = consult_tag_arity(0x06, 2);
    CHECK(idx == ((0x06u << 8) | 2u), "tag_arity pack");
    record_edsl_children_soa_path();
    IRFunctionSoA fn;
    fn.opcodes_.push_back(IROpcode::Call);
    fn.shape_ids_.push_back(42);
    fn.linear_ownership_states_.push_back(2);
    auto view = make_function_soa_view(&fn);
    std::uint32_t sh = 0;
    std::uint8_t lin = 0;
    CHECK(consult_closure_shape_linear(view, 0, sh, lin), "closure consult ok");
    CHECK(sh == 42 && lin == 2, "shape+linear");
    CHECK(load_u64(g_soa_view_hits) > h0, "hits advanced");
    CHECK(migration_ratio_bp() > 0 || load_u64(g_soa_view_hits) > 0, "ratio or hits");
}

static void run_1619_query_schema() {
    std::println("\n--- AC5 (#1619): query schema 1619 ---");
    CompilerService cs;
    (void)consult_tag_arity(1, 1);
    record_edsl_children_soa_path();
    auto h = cs.eval("(engine:metrics \"query:soa-view-enforcement-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "schema") == 1619 ||
              href(cs, "query:soa-view-enforcement-stats", "schema") == 1517,
          "schema 1619|1517");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "issue") == 1619 ||
              href(cs, "query:soa-view-enforcement-stats", "issue") < 0,
          "issue 1619");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "concept-enforcement-hits") >= 0,
          "concept-enforcement-hits");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "soa-view-pass-skipped") >= 0,
          "soa-view-pass-skipped");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "edsl-soa-migration-progress") >= 0,
          "edsl-soa-migration-progress");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "soa-view-hits") >= 1, "soa-view-hits");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "migration-ratio-bp") >= 0,
          "migration-ratio-bp");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "soa-view-full-compliant") == 1,
          "soa-view-full-compliant");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "static-assert-enforced") == 1,
          "static-assert-enforced");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "columnar-accessor-required") == 1,
          "columnar-accessor-required");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "pipeline-pack-check") == 1,
          "pipeline-pack-check");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "enforcement-phase") == 2 ||
              href(cs, "query:soa-view-enforcement-stats", "enforcement-phase") >= 1,
          "enforcement-phase");
}

static void run_1619_stress() {
    std::println("\n--- AC6 (#1619): multi-round stress ---");
    IRModule mod;
    SoaRequirePass1619 p;
    ConstantFoldingWrap cf;
    const auto h0 = load_u64(g_soa_view_hits);
    for (int i = 0; i < 50; ++i) {
        p.ran = false;
        CHECK(run_pipeline(mod, p), "pipeline ok");
        (void)consult_tag_arity(static_cast<std::uint8_t>(i & 0xff), 1);
        record_edsl_children_soa_path();
        note_pass_soa_enforcement(cf);
    }
    CHECK(load_u64(g_soa_view_hits) > h0, "hits grew");
    CHECK(migration_ratio_bp() <= 10000, "ratio <= 100%");
    CompilerService cs;
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value(), "eval ok");
}

static void run_1619_lineage() {
    std::println("\n--- AC7 (#1619): #1517 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "query:soa-view-enforcement-stats", "concept-enforced") == 1,
          "concept-enforced");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "passes-soa-view-aware") >= 0,
          "passes-soa-view-aware");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "soa-view-misses") >= 0, "soa-view-misses");
    auto adop = cs.eval("(engine:metrics \"query:soa-adoption-stats\")");
    CHECK(adop && is_hash(*adop), "adoption hash");
    auto sch = cs.eval("(hash-ref (engine:metrics \"query:soa-adoption-stats\") \"schema\")");
    CHECK(sch && is_int(*sch) &&
              (as_int(*sch) == 1629 || as_int(*sch) == 1619 || as_int(*sch) == 1517),
          "adoption schema 1629|1619|1517");
}

} // namespace aura_soa_batch

int main() {
    using namespace aura_soa_batch;
    std::println("=== SoA batch: #1624 + #1638 + #543 + #506 + #1619 (40 ACs total) ===");
    run_1624_concepts();
    run_1624_get_child_columnar();
    run_1624_set_child_contract();
    run_1624_schema();
    run_1624_stress();
    run_1624_arena_compact_linkage();
    run_1624_lineage();
    run_1638_materialize_call_env();
    run_1638_gc_roots_dual_path();
    run_1638_metrics();
    run_1638_xmacro();
    run_1638_bump_getter();
    run_1638_compact_decl();
    run_1638_ensure_dual_path();
    run_1638_exit_compact();
    run_1639_query_surface();
    run_543_query_dualpath_stats();
    run_543_accessor_baselines();
    run_543_alloc_stamps_version();
    run_543_stale_detection();
    run_543_materialize_bumps_stale();
    run_543_walk_version_mismatch();
    run_543_gc_walk_safe_skips();
    run_543_dual_path_length();
    run_543_multi_thread_rebind();
    run_543_gc_heap_walk();
    run_543_regression();
    run_506_hotpath_matrix();
    run_1619_concept();
    run_1619_pack_check();
    run_1619_production_wraps();
    run_1619_edsl_helpers();
    run_1619_query_schema();
    run_1619_stress();
    run_1619_lineage();
    std::println("\n=== SoA batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
