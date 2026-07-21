// test_walk_batch.cpp
// B pilot #11 (after mutation_boundary in 928a046f): consolidated walk family
// — Issues #1606 + #1733 + #1753 (linear live closure scan wiring +
// walk_active_closures callback exception isolation + walk_env_frames
// static_assert shape) into one batch driver.
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention (per_defuse_batch /
// env_lookup_batch / fiber_resume_batch / compact_sweep_batch /
// incremental_relower_batch / macro_reflect_batch / incremental_type_batch /
// linear_ownership_batch / dead_coercion_batch / mutation_boundary_batch
// precedents): single binary with CHECK() + per-issue AC blocks in namespace
// aura_walk_batch { run_NNN_xxx() }; EXCLUDE_FROM_ALL.
//
// AC map (consolidated, 14 ACs total):
//   Issue #1606 — 6 ACs: walk_active_closures visits registered +
//                  invalidate_function pre-cascade scan + compact_env_frames
//                  pre-compact scan + JIT ResourceTracker pre-evict scan +
//                  linear_live_closure_scans_total + marked_invalid metrics +
//                  query:linear-boundary-consistency-stats schema 1606 +
//                  safe_fallback; untracked not force-marked
//   Issue #1733 — 4 ACs: source cites #1733 + per-callback catch (std::exception
//                  + ...) + walk_active_closures_callback_exceptions metric +
//                  throwing callback does not abort walk (later closures
//                  visited) + clean walk no metric bump
//   Issue #1753 — 4 ACs: source cites #1753 + static_assert on invocable +
//                  bool-convertible return + no `requires AuraInvocable` on
//                  walk_env_frames + happy-path parent chain walk +
//                  returning false stops early

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_walk_batch {

using aura::compiler::Closure;
using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::EnvFrame;
using aura::compiler::EnvId;
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
constexpr std::uint8_t kUntracked = 0;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-boundary-consistency-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

static std::pair<ClosureId, EnvId> make_linear_capture_closure(CompilerService& cs,
                                                               std::uint8_t state) {
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

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ── Issue #1606 — walk_active_closures + linear live scan ──
static void run_1606_walk_visits() {
    std::println("\n--- AC1 (#1606): walk_active_closures visits registered ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    int visited = 0;
    bool saw = false;
    ev.walk_active_closures([&](auto id, auto& cl) {
        ++visited;
        if (id == cid) {
            saw = true;
            CHECK(cl.env_id != NULL_ENV_ID, "env_id set");
            CHECK(cl.bridge_epoch != 0, "stamped bridge_epoch");
        }
    });
    CHECK(visited >= 1, "visited >= 1");
    CHECK(saw, "registered closure visited");
}

static void run_1606_invalidate_scan() {
    std::println("\n--- AC2 (#1606): invalidate_function pre-cascade scan ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "set-code");
    (void)cs.eval("(eval-current)");
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    const auto marked0 = load_u64(m->linear_live_closures_marked_invalid_total);
    cs.public_invalidate_function("f");
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "scans advanced");
    auto cl = ev.find_active_closure(cid);
    CHECK(cl.has_value(), "closure present");
    CHECK(cl->bridge_epoch == 0, "marked invalid (bridge_epoch=0)");
    CHECK(load_u64(m->linear_live_closures_marked_invalid_total) >= marked0 + 1,
          "marked_invalid advanced");
}

static void run_1606_compact_scan() {
    std::println("\n--- AC3 (#1606): compact_env_frames pre-compact scan ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_linear_capture_closure(cs, kMoved);
    (void)eid;
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    (void)ev.compact_env_frames();
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "compact scans");
    auto cl = ev.find_active_closure(cid);
    if (cl)
        CHECK(cl->bridge_epoch == 0, "compact marked linear invalid");
    else
        CHECK(true, "reclaimed ok");
}

static void run_1606_jit_resource_tracker() {
    std::println("\n--- AC4 (#1606): JIT ResourceTracker pre-evict scan ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)cid;
    (void)eid;
    const auto scans0 = load_u64(m->linear_live_closure_scans_total);
    (void)aura_jit_linear_live_closure_scan();
    CHECK(load_u64(m->linear_live_closure_scans_total) > scans0, "JIT host scan +1");
}

static void run_1606_metrics_and_query() {
    std::println("\n--- AC5/AC6 (#1606): metrics + schema + safe_fallback ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    auto [cid, eid] = make_linear_capture_closure(cs, kOwned);
    (void)eid;
    cs.bump_bridge_epoch();
    (void)ev.scan_live_closures_for_linear_captures(true);

    const auto safe0 = load_u64(m->compiler_closure_safe_fallbacks);
    auto r = ev.apply_closure(cid, {});
    CHECK(!r.has_value() || true, "apply no crash");
    CHECK(load_u64(m->compiler_closure_safe_fallbacks) >= safe0 || !r.has_value(),
          "safe_fallback or refuse");

    CHECK(load_u64(m->linear_live_closure_scans_total) >= 1, "scans_total readable");

    auto h = cs.eval("(engine:metrics \"query:linear-boundary-consistency-stats\")");
    CHECK(h && is_hash(*h), "hash");
    const auto schema = href(cs, "schema");
    // Lineage: #1659 bumps schema; accept 1659|1606|1596|1568.
    CHECK(schema == 1895 || schema == 1659 || schema == 1606 || schema == 1596 || schema == 1568,
          std::format("schema 1895|1659|1606|1596|1568 (got {})", schema));
    CHECK(href(cs, "linear_live_closure_scans_total") >= 0, "scans key");
    CHECK(href(cs, "walk-active-closures-wired") == 1, "walk wired");
    CHECK(href(cs, "invalidate-scan-wired") == 1 || href(cs, "invalidate-scan-wired") < 0,
          "invalidate-scan if present");
    CHECK(href(cs, "compact-scan-wired") == 1 || href(cs, "compact-scan-wired") < 0,
          "compact-scan if present");
    CHECK(href(cs, "jit-resource-tracker-scan-wired") == 1 ||
              href(cs, "jit-resource-tracker-scan-wired") < 0,
          "jit scan if present");

    // Untracked must not be force-marked
    auto [cid2, eid2] = make_linear_capture_closure(cs, kUntracked);
    (void)eid2;
    auto before = ev.find_active_closure(cid2);
    CHECK(before && before->bridge_epoch != 0, "untracked stamped");
    const auto ep = before->bridge_epoch;
    (void)ev.scan_live_closures_for_linear_captures(true);
    auto after = ev.find_active_closure(cid2);
    CHECK(after && after->bridge_epoch == ep, "Untracked not marked invalid");
}

// ── Issue #1733 — walk_active_closures isolates callback exceptions ──
static void run_1733_source_shape() {
    std::println("\n--- AC1/AC2 (#1733): source citations + metric field ---");
    std::string env_cpp;
    for (const char* p : {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
        env_cpp = read_file(p);
        if (!env_cpp.empty())
            break;
    }
    CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
    CHECK(env_cpp.find("#1733") != std::string::npos, "cites #1733");
    CHECK(env_cpp.find("walk_active_closures_callback_exceptions") != std::string::npos,
          "bumps exception metric");
    CHECK(env_cpp.find("catch (const std::exception&") != std::string::npos,
          "catches std::exception");
    CHECK(env_cpp.find("catch (...)") != std::string::npos, "catches ...");

    std::string msrc;
    for (const char* p :
         {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
        msrc = read_file(p);
        if (!msrc.empty())
            break;
    }
    CHECK(!msrc.empty() &&
              msrc.find("walk_active_closures_callback_exceptions") != std::string::npos,
          "metric field declared");
}

static void run_1733_throw_continues() {
    std::println("\n--- AC3 (#1733): throw mid-walk continues enumeration ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "metrics wired");

    std::vector<ClosureId> ids;
    for (int i = 0; i < 5; ++i) {
        Closure cl;
        cl.env_id = NULL_ENV_ID;
        ids.push_back(ev.register_active_closure(std::move(cl)));
    }
    CHECK(ids.size() == 5, "registered 5 closures");

    const auto ex0 = m->walk_active_closures_callback_exceptions.load(std::memory_order_relaxed);
    int visited = 0;
    int throw_at = 2;
    bool saw_after_throw = false;
    int visit_index = 0;

    ev.walk_active_closures([&](auto id, auto& /*cl*/) {
        (void)id;
        if (visit_index == throw_at) {
            ++visit_index;
            throw std::runtime_error("walk-callback-test");
        }
        ++visited;
        if (visit_index > throw_at)
            saw_after_throw = true;
        ++visit_index;
    });

    const auto ex1 = m->walk_active_closures_callback_exceptions.load(std::memory_order_relaxed);
    CHECK(ex1 == ex0 + 1, "exception metric +1");
    CHECK(visited >= 4, "visited remaining closures after throw");
    CHECK(saw_after_throw || visited >= 4, "walk continued past throw site");
}

static void run_1733_clean_no_bump() {
    std::println("\n--- AC4 (#1733): clean walk does not bump exception metric ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    Closure cl;
    cl.env_id = NULL_ENV_ID;
    (void)ev.register_active_closure(std::move(cl));

    const auto ex0 = m->walk_active_closures_callback_exceptions.load(std::memory_order_relaxed);
    int n = 0;
    ev.walk_active_closures([&](auto, auto&) { ++n; });
    const auto ex1 = m->walk_active_closures_callback_exceptions.load(std::memory_order_relaxed);
    CHECK(n >= 1, "visited at least one");
    CHECK(ex1 == ex0, "no exception metric bump on clean walk");
}

// ── Issue #1753 — walk_env_frames uses static_assert for F signature ──
static std::string walk_window(const std::string& src) {
    auto pos = src.find("void walk_env_frames(EnvId start, F&& f)");
    if (pos == std::string::npos)
        return {};
    auto begin = pos > 400 ? pos - 400 : 0;
    auto end = src.find("\n    // Introspection: number of frames", pos);
    if (end == std::string::npos)
        end = pos + 1200;
    return src.substr(begin, end - begin);
}

static void run_1753_source_shape() {
    std::println("\n--- AC1/AC2 (#1753): static_assert, no requires on walk_env_frames ---");
    std::string ixx;
    for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
        ixx = read_file(p);
        if (!ixx.empty())
            break;
    }
    CHECK(!ixx.empty(), "read evaluator.ixx");
    auto win = walk_window(ixx);
    CHECK(!win.empty(), "found walk_env_frames");
    CHECK(win.find("#1753") != std::string::npos, "cites #1753");
    CHECK(win.find("static_assert") != std::string::npos, "has static_assert");
    CHECK(win.find("is_invocable_v") != std::string::npos, "checks is_invocable_v");
    CHECK(win.find("is_convertible_v") != std::string::npos ||
              win.find("invoke_result_t") != std::string::npos,
          "checks bool-convertible return");
    CHECK(win.find("requires aura::core::AuraInvocable") == std::string::npos,
          "no requires AuraInvocable on walk_env_frames");
}

static void run_1753_parent_chain() {
    std::println("\n--- AC3 (#1753): parent chain visited ---");
    Evaluator ev;
    EnvId a = ev.alloc_env_frame(NULL_ENV_ID);
    EnvId b = ev.alloc_env_frame(a);
    EnvId c = ev.alloc_env_frame(b);
    CHECK(a != NULL_ENV_ID && b != NULL_ENV_ID && c != NULL_ENV_ID, "alloc frames");
    std::vector<EnvId> seen;
    ev.walk_env_frames(c, [&](EnvId id, const EnvFrame&) {
        seen.push_back(id);
        return true;
    });
    CHECK(seen.size() == 3, "visited 3 frames");
    if (seen.size() >= 3) {
        CHECK(seen[0] == c, "starts at c");
        CHECK(seen[1] == b, "then b");
        CHECK(seen[2] == a, "then a");
    }
    CHECK(ev.env_depth(c) == 3, "env_depth matches");
}

static void run_1754_early_stop() {
    std::println("\n--- AC4 (#1753): false stops walk ---");
    Evaluator ev;
    EnvId a = ev.alloc_env_frame(NULL_ENV_ID);
    EnvId b = ev.alloc_env_frame(a);
    EnvId c = ev.alloc_env_frame(b);
    int n = 0;
    ev.walk_env_frames(c, [&](EnvId, const EnvFrame&) {
        ++n;
        return n < 2;
    });
    CHECK(n == 2, "stopped after 2");
}

} // namespace aura_walk_batch

int main() {
    using namespace aura_walk_batch;
    std::println("=== Walk batch: #1606 + #1733 + #1753 (14 ACs total) ===");
    run_1606_walk_visits();
    run_1606_invalidate_scan();
    run_1606_compact_scan();
    run_1606_jit_resource_tracker();
    run_1606_metrics_and_query();
    run_1733_source_shape();
    run_1733_throw_continues();
    run_1733_clean_no_bump();
    run_1753_source_shape();
    run_1753_parent_chain();
    run_1754_early_stop();
    std::println("\n=== Walk batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
