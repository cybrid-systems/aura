// @category: integration
// @reason: Issue #1928 — walk_active_closures + force linear Drop on
// invalidate/compact/truncate/JIT/fiber/GC boundaries (refine #1895).
//
//   AC1: source cites #1928; walk_active_closures + scan force Drop
//   AC2: schema-1928 on query:linear-boundary-consistency-stats
//   AC3: walk visits registered closures under lock
//   AC4: scan force-Drop Moved + NULL_ENV_ID
//   AC5: 5+ boundary wire flags (invalidate/compact/trunc/jit/fiber/gc)
//   AC6: truncate path advances linear_live_closure_scans_total
//   AC7: multi-boundary stress counters grow; eval still works
//   AC8: #1895 lineage schema retained

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr std::uint8_t kOwned = 1;
constexpr std::uint8_t kMoved = 4;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-boundary-consistency-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static auto make_linear_closure(CompilerService& cs, std::uint8_t state) {
    auto& ev = cs.evaluator();
    if (ev.current_bridge_epoch() == 0)
        cs.bump_bridge_epoch();
    aura::compiler::Env src;
    src.bind_symid_with_linear_state(42, make_int(7), state);
    auto eid = ev.alloc_env_frame_from_env(src);
    Closure cl;
    cl.env_id = eid;
    auto cid = ev.register_active_closure(std::move(cl));
    return std::pair{cid, eid};
}

static auto make_null_env_live_closure(CompilerService& cs) {
    auto& ev = cs.evaluator();
    if (ev.current_bridge_epoch() == 0)
        cs.bump_bridge_epoch();
    Closure cl;
    cl.env_id = NULL_ENV_ID;
    return ev.register_active_closure(std::move(cl));
}

static void ac1_source() {
    std::println("\n--- AC1: #1928 source surface ---");
    auto env = read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
    auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    CHECK(!env.empty() && env.find("#1928") != std::string::npos, "env cites #1928");
    CHECK(env.find("walk_active_closures") != std::string::npos, "walk");
    CHECK(env.find("scan_live_closures_for_linear_captures") != std::string::npos, "scan");
    // truncate path must force Drop before resize
    CHECK(env.find("BEFORE resize") != std::string::npos ||
              env.find("pre-truncate") != std::string::npos ||
              env.find("mark_invalid=*/true") != std::string::npos,
          "truncate force Drop");
    CHECK(!ixx.empty() && ixx.find("walk_active_closures") != std::string::npos, "ixx API");
}

static void ac2_schema() {
    std::println("\n--- AC2: schema-1928 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:linear-boundary-consistency-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1895, "lineage 1895");
    CHECK(href(cs, "schema-1928") == 1928, "schema-1928");
    CHECK(href(cs, "issue-1928") == 1928, "issue-1928");
    CHECK(href(cs, "linear_live_closure_scans_total") >= 0, "scans");
    CHECK(href(cs, "linear_ownership_violation_prevented") >= 0, "prevented");
    CHECK(href(cs, "linear_gc_root_audit_checks_total") >= 0, "gc audit");
    CHECK(href(cs, "boundaries-wired-count") == 6, "6 boundaries");
    CHECK(href(cs, "truncate-scan-wired") == 1, "truncate wired");
}

static void ac3_walk() {
    std::println("\n--- AC3: walk_active_closures ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto [cid, eid] = make_linear_closure(cs, kOwned);
    (void)eid;
    int n = 0;
    bool saw = false;
    ev.walk_active_closures([&](auto id, auto& cl) {
        ++n;
        if (id == cid) {
            saw = true;
            CHECK(cl.env_id != NULL_ENV_ID, "env set");
        }
    });
    CHECK(n >= 1 && saw, "walk visits");
}

static void ac4_force_drop() {
    std::println("\n--- AC4: force Drop Moved + NULL_ENV ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_linear_closure(cs, kMoved);
    (void)eid;
    const auto prev = load_u64(m->linear_ownership_violation_prevented);
    auto r = ev.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                       /*only_if_moved=*/true);
    CHECK(r.with_moved_capture >= 1, "found Moved");
    CHECK(r.marked_invalid >= 1, "marked");
    auto cl = ev.find_active_closure(cid);
    CHECK(cl && cl->bridge_epoch == 0, "force Drop");
    CHECK(load_u64(m->linear_ownership_violation_prevented) > prev, "prevented");

    auto null_cid = make_null_env_live_closure(cs);
    const auto null0 = load_u64(m->linear_null_env_force_drop_total);
    (void)ev.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                    /*only_if_moved=*/false);
    auto ncl = ev.find_active_closure(null_cid);
    CHECK(ncl && ncl->bridge_epoch == 0, "NULL_ENV force Drop");
    CHECK(load_u64(m->linear_null_env_force_drop_total) > null0, "null metric");
}

static void ac5_wire_flags() {
    std::println("\n--- AC5: 5+ boundary wire flags ---");
    CompilerService cs;
    CHECK(href(cs, "walk-active-closures-wired") == 1, "walk");
    CHECK(href(cs, "invalidate-scan-wired") == 1, "invalidate");
    CHECK(href(cs, "compact-scan-wired") == 1, "compact");
    CHECK(href(cs, "truncate-scan-wired") == 1, "truncate");
    CHECK(href(cs, "jit-resource-tracker-scan-wired") == 1, "jit");
    CHECK(href(cs, "fiber-steal-scan-wired") == 1, "fiber");
    CHECK(href(cs, "gc-safepoint-scan-wired") == 1, "gc");
    CHECK(href(cs, "force-drop-wired") == 1, "force drop");
}

static void ac6_truncate_scans() {
    std::println("\n--- AC6: truncate advances scans ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    (void)make_linear_closure(cs, kMoved);
    (void)make_null_env_live_closure(cs);
    for (int i = 0; i < 3; ++i)
        (void)ev.alloc_env_frame();
    const auto base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base);
    (void)ev.alloc_env_frame(); // past checkpoint
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    CHECK(ev.truncate_env_frames_to_checkpoint() >= 1, "truncated");
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "scans advanced");
}

static void ac7_stress() {
    std::println("\n--- AC7: multi-boundary stress ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    for (int i = 0; i < 48; ++i) {
        (void)make_linear_closure(cs, (i % 2) == 0 ? kMoved : kOwned);
        if ((i % 4) == 0)
            (void)make_null_env_live_closure(cs);
        if ((i % 3) == 0)
            (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditGcSafepoint,
                                                         true);
        if ((i % 5) == 0)
            (void)ev.compact_env_frames();
        if ((i % 7) == 0)
            (void)ev.probe_linear_ownership_on_fiber_steal();
    }
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "scans grew");
    CHECK(href(cs, "schema-1928") == 1928, "schema holds");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval after stress");
}

static void ac8_lineage() {
    std::println("\n--- AC8: #1895 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "schema") == 1895, "schema 1895");
    CHECK(href(cs, "walk-active-closures-wired") == 1, "walk");
    CHECK(href(cs, "active") == 1, "active");
}

} // namespace

int main() {
    std::println("=== Issue #1928: walk_active_closures linear boundary mandate ===");
    ac1_source();
    ac2_schema();
    ac3_walk();
    ac4_force_drop();
    ac5_wire_flags();
    ac6_truncate_scans();
    ac7_stress();
    ac8_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
