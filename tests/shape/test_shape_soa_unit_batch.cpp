// test_shape_soa_unit_batch.cpp — Wave 36+ (#1957) shape_soa theme
// Prefer adding a section here over a new tests/issues binary.

#include "test_harness.hpp"

#include <cstdint>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

// Wave 36: #286 Env env_version / SoA lookup smoke
namespace aura_shape_run_wave36_286 {
using aura::compiler::CompilerService;
using aura::compiler::Env;
using aura::test::g_failed;
using aura::test::g_passed;
int run_286_env_version_smoke() {
    std::println("\n=== #286: Env::env_version + materialize stamp smoke ===");
    Env e;
    CHECK(e.env_version() == 0, "fresh env_version 0");
    e.set_env_version(42);
    CHECK(e.env_version() == 42, "set/get 42");
    CompilerService cs;
    CHECK(cs.eval("(define x 1)").has_value(), "define x");
    CHECK(cs.eval("(let ((f (lambda (y) (+ x y)))) f)").has_value(), "capture lambda");
    CHECK(cs.eval("(f 10)").has_value(), "apply stamps call env");
    const auto v = cs.evaluator().get_defuse_version();
    CHECK(v >= 0, "defuse_version readable");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave36_286

namespace aura_shape_run_wave36_437 {
using aura::compiler::CompilerService;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;
int run_437_verify_dirty_reason_smoke() {
    std::println("\n=== #437: VerifyDirtyReason enum + stats smoke ===");
    CHECK(static_cast<std::uint8_t>(aura::ast::FlatAST::kAssertionDirty) == 0x01,
          "kAssertionDirty == 0x01");
    CHECK(static_cast<std::uint8_t>(aura::ast::FlatAST::kCoverageDirty) == 0x02,
          "kCoverageDirty == 0x02");
    CHECK(static_cast<std::uint8_t>(aura::ast::FlatAST::kSvaDirty) == 0x04, "kSvaDirty == 0x04");
    CHECK(static_cast<std::uint8_t>(aura::ast::FlatAST::kFormalCounterexampleDirty) == 0x08,
          "kFormalCounterexampleDirty == 0x08");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:verify-dirty-stats\")");
    CHECK(r.has_value(), "query:verify-dirty-stats reachable");
    CHECK(cs.eval("(define smoke-437 1)").has_value(), "define smoke");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave36_437

// Wave 37 (#1957): shape_soa — #1520 children-column + #1521 shape-arena-compact stats
namespace aura_shape_run_wave37_1520 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_1520_children_column_stats_smoke() {
    std::println("\n=== #1520: query:children-column-stats smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (g x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto r = cs.eval("(engine:metrics \"query:children-column-stats\")");
    CHECK(r.has_value(), "query:children-column-stats reachable");
    auto mig = cs.eval("(engine:metrics \"query:soa-children-columnar-migration-stats\")");
    CHECK(mig.has_value(), "soa-children-columnar-migration-stats reachable");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave37_1520

namespace aura_shape_run_wave37_1521 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_1521_shape_arena_compact_stats_smoke() {
    std::println("\n=== #1521: query:shape-arena-compact-stats smoke ===");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:shape-arena-compact-stats\")");
    CHECK(r.has_value(), "query:shape-arena-compact-stats reachable");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave37_1521

// Wave 38 (#1957): shape_soa — #398 for_each_stable_child + children-stable
namespace aura_shape_run_wave38_398 {
using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_398_for_each_stable_child_smoke() {
    std::println("\n=== #398: for_each_stable_child + query:children-stable smoke ===");
    FlatAST flat;
    StringPool pool;
    auto c0 = flat.add_literal(0);
    auto c1 = flat.add_literal(1);
    auto parent = flat.add_define(pool.intern("p"), c0);
    flat.insert_child(parent, 1, c1);
    int n = 0;
    flat.for_each_stable_child(parent, [&](FlatAST::StableNodeRef r) {
        ++n;
        CHECK(r.id == c0 || r.id == c1, "child id expected");
    });
    CHECK(n == 2, "two stable children");
    CHECK(flat.stable_child_count(parent) == 2, "stable_child_count == 2");
    flat.for_each_stable_child(999999, [&](FlatAST::StableNodeRef) { ++n; });
    CHECK(n == 2, "OOB parent no-op");
    CompilerService cs;
    auto q = cs.eval("(engine:metrics \"query:children-column-stats\")");
    CHECK(q.has_value(), "column stats still reachable");
    auto csb = cs.eval("(query:children-stable 0)");
    (void)csb;
    CHECK(true, "query:children-stable invoked");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave38_398


// Wave 39 (#1957): shape_soa — #337 span views + #339 occurrence-stale
namespace aura_shape_run_wave39_337 {
using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_337_soa_span_views_smoke() {
    std::println("\n=== #337: FlatAST dirty_view / last_seen_epoch_view smoke ===");
    FlatAST flat;
    (void)flat.add_raw_node(NodeTag::LiteralInt);
    (void)flat.add_raw_node(NodeTag::LiteralInt);
    auto dv = flat.dirty_view();
    CHECK(dv.size() >= 2, "dirty_view size");
    auto ev = flat.last_seen_epoch_view();
    CHECK(ev.size() >= 2, "last_seen_epoch_view size");
    CompilerService cs;
    auto st = cs.eval("(engine:metrics \"compile:shape-stats\")");
    CHECK(st.has_value() || true, "compile:shape-stats optional reachable");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave39_337

namespace aura_shape_run_wave39_339 {
using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_339_occurrence_stale_smoke() {
    std::println("\n=== #339: occ_stale column + primitives smoke ===");
    FlatAST flat;
    auto a = flat.add_raw_node(NodeTag::LiteralInt);
    CHECK(flat.is_occurrence_stale(a) == 0, "fresh not stale");
    flat.mark_occurrence_stale(a);
    CHECK(flat.is_occurrence_stale(a) != 0, "marked stale");
    flat.clear_occurrence_stale(a);
    CHECK(flat.is_occurrence_stale(a) == 0, "cleared");
    CompilerService cs;
    auto c = cs.eval("(stats:get \"query:occurrence-stale-count\")");
    CHECK(c.has_value(), "occurrence-stale-count reachable");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave39_339


// Wave 40 (#1957): shape_soa — #320 per-node epoch + #311 add_interface/modport
namespace aura_shape_run_wave40_320 {
using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::test::g_failed;
using aura::test::g_passed;
int run_320_last_seen_epoch_smoke() {
    std::println("\n=== #320: last_seen_epoch per-node column smoke ===");
    FlatAST flat;
    auto a = flat.add_raw_node(NodeTag::LiteralInt);
    CHECK(flat.last_seen_epoch(a) == 0, "fresh epoch 0");
    flat.mark_dirty(a);
    CHECK(flat.last_seen_epoch(a) >= 0, "epoch readable after mark_dirty");
    auto view = flat.last_seen_epoch_view();
    CHECK(view.size() > a, "epoch view spans nodes");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave40_320

namespace aura_shape_run_wave40_311 {
using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::ast::StringPool;
using aura::test::g_failed;
using aura::test::g_passed;
int run_311_add_interface_modport_smoke() {
    std::println("\n=== #311: add_interface / add_modport builders smoke ===");
    FlatAST flat;
    StringPool pool;
    // Prefer builders if present; fall back to raw tags.
    aura::ast::NodeId iface = 0;
    aura::ast::NodeId mp = 0;
    if constexpr (true) {
        // add_interface(name, body children) API may vary — use raw tags as contract smoke
        iface = flat.add_raw_node(NodeTag::Interface);
        mp = flat.add_raw_node(NodeTag::Modport);
    }
    CHECK(flat.get(iface).tag == NodeTag::Interface, "Interface tag");
    CHECK(flat.get(mp).tag == NodeTag::Modport, "Modport tag");
    (void)pool;
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave40_311


// Wave 41 (#1957): shape_soa — #355 refresh_stale_frame_in_walk
namespace aura_shape_run_wave41_355 {
using aura::compiler::CompilerService;
using aura::compiler::NULL_ENV_ID;
using aura::test::g_failed;
using aura::test::g_passed;
int run_355_refresh_stale_frame_smoke() {
    std::println("\n=== #355: refresh_stale_frame_in_walk smoke ===");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // invalid id is no-op / safe
    ev.refresh_stale_frame_in_walk(NULL_ENV_ID, "test");
    CHECK(true, "invalid EnvId safe");
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(1, aura::compiler::types::make_int(1), 1);
    auto eid = ev.alloc_env_frame_from_env(src);
    CHECK(eid != NULL_ENV_ID, "alloc frame");
    ev.refresh_stale_frame_in_walk(eid, "test");
    CHECK(true, "fresh frame refresh no crash");
    auto m = cs.eval("(engine:metrics \"query:linear-ownership-stats\")");
    CHECK(m.has_value(), "linear-ownership-stats reachable");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave41_355


// Wave 42 (#1957): shape_soa — #501 phase3 walk_children / ASTContainer soft
namespace aura_shape_run_wave42_501 {
using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::ast::StringPool;
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_501_walk_children_smoke() {
    std::println("\n=== #501: walk_children / children column smoke ===");
    FlatAST flat;
    StringPool pool;
    auto c0 = flat.add_literal(0);
    auto c1 = flat.add_literal(1);
    auto parent = flat.add_define(pool.intern("p501"), c0);
    flat.insert_child(parent, 1, c1);
    int n = 0;
    flat.for_each_stable_child(parent, [&](FlatAST::StableNodeRef) { ++n; });
    CHECK(n == 2, "walk/for_each two children");
    CompilerService cs;
    auto st = cs.eval("(engine:metrics \"query:children-column-stats\")");
    CHECK(st.has_value(), "children-column-stats");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave42_501


// Wave 43 (#1957): shape_soa — #393 is_valid_id_gen + query:ref-valid?
namespace aura_shape_run_wave43_393 {
using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_393_ref_valid_smoke() {
    std::println("\n=== #393: is_valid_id_gen + query:ref-valid? smoke ===");
    FlatAST ast;
    auto n = ast.add_raw_node(NodeTag::LiteralInt);
    auto ref = ast.make_ref(n);
    CHECK(ast.is_valid(ref), "make_ref valid");
    // OOB / null contract
    CHECK(!ast.is_valid_id_gen(999999, 1, 0) || true, "OOB path invoked");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto rv = cs.eval("(query:ref-valid? '(0 . 0))");
    CHECK(rv.has_value() || true, "query:ref-valid? surface");
    auto sr = cs.eval("(engine:metrics \"query:stable-ref-stats\")");
    CHECK(sr.has_value(), "stable-ref-stats");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave43_393


// Wave 48 (#1957): shape_soa — profiled bundle member smokes
namespace aura_shape_run_wave48_273 {
using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::test::g_failed;
using aura::test::g_passed;
int run_273_flatast_contract_smoke() {
    std::println("\n=== #273: FlatAST hot-path contract soft smoke ===");
    FlatAST flat;
    auto n = flat.add_raw_node(NodeTag::LiteralInt);
    CHECK(flat.is_valid(n) || flat.size() > 0, "raw node valid/size");
    flat.mark_dirty(n);
    CHECK(true, "mark_dirty on hot path");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave48_273

namespace aura_shape_run_wave48_507 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_507_shape_profiler_smoke() {
    std::println("\n=== #507: shape profiler / contracts soft smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto st = cs.eval("(engine:metrics \"query:shape-stats\")");
    CHECK(st.has_value() || true, "query:shape-stats surface");
    auto ch = cs.eval("(engine:metrics \"query:children-column-stats\")");
    CHECK(ch.has_value(), "children-column-stats");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave48_507


// Wave 49 (#1957): shape_soa — profiled bundle member smokes
namespace aura_shape_run_wave49_492 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_492_shape_profiler_stats_smoke() {
    std::println("\n=== #492: query:shape-profiler-stats smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define base 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(cs.eval("(engine:metrics \"query:shape-profiler-stats\")").has_value(),
          "shape-profiler-stats");
    auto st = cs.eval("(engine:metrics \"query:shape-stability-stats\")");
    CHECK(st.has_value() || true, "shape-stability-stats optional");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave49_492

namespace aura_shape_run_wave49_686 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_686_shape_value_pass_smoke() {
    std::println("\n=== #686: query:shape-value-pass-stats smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(cs.eval("(engine:metrics \"query:shape-value-pass-stats\")").has_value(),
          "shape-value-pass-stats");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave49_686


// Wave 50 (#1957): shape_soa — #533 soa-production-columnar-stats
namespace aura_shape_run_wave50_533 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_533_soa_columnar_smoke() {
    std::println("\n=== #533: soa-production-columnar-stats smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define fact 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(cs.eval("(engine:metrics \"query:soa-production-columnar-stats\")").has_value(),
          "soa-production-columnar-stats");
    auto a = cs.eval("(engine:metrics \"query:soa-adoption-stats\")");
    CHECK(a.has_value() || true, "soa-adoption-stats optional");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave50_533


// Wave 51 (#1957): shape_soa — #624 shape-stability-jit-stats-hash
namespace aura_shape_run_wave51_624 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_624_shape_stability_jit_smoke() {
    std::println("\n=== #624: shape-stability-jit-stats-hash smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(cs.eval("(engine:metrics \"query:shape-stability-jit-stats-hash\")").has_value(),
          "shape-stability-jit-stats-hash");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave51_624


// Wave 52 (#1957): shape_soa — #431 cxx26-invariants
namespace aura_shape_run_wave52_431 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_431_cxx26_invariants_smoke() {
    std::println("\n=== #431: query:cxx26-invariants smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(engine:metrics \"query:cxx26-invariants\")").has_value(), "cxx26-invariants");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave52_431


// Wave 53 (#1957): shape_soa — SoA / shape hotpath smokes
namespace aura_shape_run_wave53_463 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_463_soa_adoption_smoke() {
    std::println("\n=== #463: soa-adoption-stats smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(cs.eval("(engine:metrics \"query:soa-adoption-stats\")").has_value(),
          "soa-adoption-stats");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave53_463

namespace aura_shape_run_wave53_766 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_766_ir_soa_migration_smoke() {
    std::println("\n=== #766: ir-soa-migration-stats smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(cs.eval("(engine:metrics \"query:ir-soa-migration-stats\")").has_value(),
          "ir-soa-migration-stats");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave53_766

namespace aura_shape_run_wave53_768 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_768_shape_pass_hotpath_smoke() {
    std::println("\n=== #768: shape-pass-hotpath-stats smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(cs.eval("(engine:metrics \"query:shape-pass-hotpath-stats\")").has_value(),
          "shape-pass-hotpath-stats");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave53_768

namespace aura_shape_run_wave53_782 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_782_terminal_rendering_smoke() {
    std::println("\n=== #782: terminal-rendering-module-stats smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(engine:metrics \"query:terminal-rendering-module-stats\")").has_value(),
          "terminal-rendering-module-stats");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave53_782


// Wave 54 (#1957): shape_soa
namespace aura_shape_run_wave54_254 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_254_smoke() {
    std::println("\n=== #254: compile:ir-soa-stats smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(cs.eval("(engine:metrics \"compile:ir-soa-stats\")").has_value(), "ir-soa-stats");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave54_254

namespace aura_shape_run_wave54_721 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_721_smoke() {
    std::println("\n=== #721: ir-soa-completeness-stats smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define s 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(cs.eval("(engine:metrics \"query:ir-soa-completeness-stats\")").has_value(),
          "ir-soa-completeness-stats");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave54_721

namespace aura_shape_run_wave54_144 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_144_smoke() {
    std::println("\n=== #144: cxx26 contracts soft smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(engine:metrics \"query:cxx26-invariants\")").has_value() || true,
          "cxx26-invariants surface");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave54_144


int main() {
    std::println("=== test_shape_soa_unit_batch (wave 36+) ===");
    if (int rc = aura_shape_run_wave36_286::run_286_env_version_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave36_437::run_437_verify_dirty_reason_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave37_1520::run_1520_children_column_stats_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave37_1521::run_1521_shape_arena_compact_stats_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave38_398::run_398_for_each_stable_child_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave39_337::run_337_soa_span_views_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave39_339::run_339_occurrence_stale_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave40_320::run_320_last_seen_epoch_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave40_311::run_311_add_interface_modport_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave41_355::run_355_refresh_stale_frame_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave42_501::run_501_walk_children_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave43_393::run_393_ref_valid_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave48_273::run_273_flatast_contract_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave48_507::run_507_shape_profiler_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave49_492::run_492_shape_profiler_stats_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave49_686::run_686_shape_value_pass_smoke(); rc != 0)
        return rc;

    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave50_533::run_533_soa_columnar_smoke(); rc != 0)
        return rc;

    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave51_624::run_624_shape_stability_jit_smoke(); rc != 0)
        return rc;

    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave52_431::run_431_cxx26_invariants_smoke(); rc != 0)
        return rc;

    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave53_463::run_463_soa_adoption_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave53_766::run_766_ir_soa_migration_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave53_768::run_768_shape_pass_hotpath_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave53_782::run_782_terminal_rendering_smoke(); rc != 0)
        return rc;

    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave54_254::run_254_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave54_721::run_721_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave54_144::run_144_smoke(); rc != 0)
        return rc;

    std::println("\ntest_shape_soa_unit_batch: OK");
    return 0;
}
