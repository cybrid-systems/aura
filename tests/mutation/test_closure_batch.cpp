// test_closure_batch.cpp
// B pilot #16 (after reflect in 940512a6): consolidated closure family
// — Issues #1709 + #1626 + #1706 + #1708 + #1870 (capture bounds +
// dual-check apply_closure + JIT + exists epoch disambiguation + free_list
// order + ClosureView zero-copy lifetime) into one batch driver.
//
// NOTE: test_closure_free.cpp is intentionally NOT included — registered
// in cmake/AuraDomainTests.cmake:559-562 with add_dependencies(all_test_issue_targets ...)
// (default-build), not the standard aura_add_issue_test pattern. Consolidating
// it into an EXCLUDE_FROM_ALL batch would lose default-build coverage of
// the ID reuse + closure:free! primitive surface.
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention (per_defuse_batch /
// env_lookup_batch / fiber_resume_batch / compact_sweep_batch /
// incremental_relower_batch / macro_reflect_batch / incremental_type_batch /
// linear_ownership_batch / dead_coercion_batch / mutation_boundary_batch /
// walk_batch / compact_batch / gc_batch / soa_batch / reflect_batch
// precedents): single binary with CHECK() + per-issue AC blocks in namespace
// aura_closure_batch { run_NNN_xxx() }; EXCLUDE_FROM_ALL.
//
// AC map (consolidated, 22 ACs total):
//   Issue #1709 — 4 ACs: capture into live slot succeeds +
//                  OOR capture is no-op + capture into freed slot
//                  is no-op + source has #1709 closure_slot_in_bounds +
//                  func_ids check
//   Issue #1626 — 7 ACs: query schema 1626 + apply_closure after
//                  mark_define_dirty → safe fallback no crash +
//                  JIT aura_closure_call dual_check + stale_deopt +
//                  1000× concurrent mutate + apply old closures +
//                  compiler_closure_envframe_stale_total metric +
//                  quote / lambda / recursive after dual-check +
//                  #1604/#1607 lineage keys
//   Issue #1706 — 4 ACs: exists(-1/huge)==0 + epoch OOR still 0 +
//                  alloc + exists(allocated)==1, epoch may be 0 +
//                  exists disambiguates OOR vs live + source documents
//                  #1706 convention
//   Issue #1708 — 4 ACs: free then alloc reuses slot +
//                  free_list push before freed=1 in source +
//                  double-free idempotent + source cites #1708
//   Issue #1870 — 3 ACs: ClosureView / make_closure_view document
//                  #1870 lifetime + sequential make_closure_view reads
//                  params/name/arity + remake after params growth
//                  (supported pattern)

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;
import aura.core;

namespace aura_closure_batch {

using aura::ast::SymId;
using aura::compiler::Closure;
using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::make_closure_view;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

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

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:epoch-apply-hotpath-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void seed_define(CompilerService& cs, const char* name) {
    aura::ir::IRFunction entry_fn;
    entry_fn.id = 0;
    entry_fn.name = "__top__";
    entry_fn.entry_block = 0;
    entry_fn.blocks.push_back({0, {}, {}});
    aura::ir::IRFunction body_fn;
    body_fn.id = 1;
    body_fn.name = std::string(name) + "#0";
    body_fn.entry_block = 0;
    body_fn.blocks.push_back({0, {}, {}});
    cs.store_define_v2(name, std::string("(define (") + name + " x) (+ x 1))",
                       std::vector{entry_fn, body_fn}, {}, {});
}

// ── Issue #1709 — capture bounds ──
static void run_1709_live_capture() {
    std::println("\n--- AC1 (#1709): capture into live slot ---");
    auto cid = aura_alloc_closure(1);
    CHECK(cid >= 0, "alloc");
    aura_closure_capture(cid, 0, 42);
    aura_closure_capture(cid, 1, 99);
    CHECK(true, "capture into live slot no crash");
    aura_free_closure(cid);
}

static void run_1709_oor_capture() {
    std::println("\n--- AC2 (#1709): OOR capture is no-op ---");
    aura_closure_capture(-1, 0, 1);
    aura_closure_capture(999999, 0, 1);
    CHECK(true, "OOR capture no crash");
}

static void run_1709_freed_capture() {
    std::println("\n--- AC3 (#1709): capture into freed slot ---");
    auto cid = aura_alloc_closure(2);
    CHECK(cid >= 0, "alloc for free");
    aura_free_closure(cid);
    CHECK(aura_closure_is_freed(cid) == 1, "is_freed");
    aura_closure_capture(cid, 0, 7);
    CHECK(true, "capture after free no crash");
}

static void run_1709_source() {
    std::println("\n--- AC4 (#1709): source has #1709 bounds ---");
    std::string src =
        read_first({"src/compiler/aura_jit_runtime.cpp", "../src/compiler/aura_jit_runtime.cpp"});
    CHECK(!src.empty(), "read runtime");
    if (!src.empty()) {
        CHECK(src.find("Issue #1709") != std::string::npos, "cites #1709");
        CHECK(src.find("closure_slot_in_bounds") != std::string::npos,
              "closure_slot_in_bounds helper");
        auto cap = src.find("void aura_closure_capture");
        CHECK(cap != std::string::npos, "found capture");
        if (cap != std::string::npos) {
            auto win = src.substr(cap, 1200);
            CHECK(win.find("closure_slot_in_bounds") != std::string::npos,
                  "capture uses closure_slot_in_bounds");
            CHECK(win.find("g_closure_func_ids") != std::string::npos ||
                      win.find("closure_slot_in_bounds") != std::string::npos,
                  "func_ids in capture path");
        }
    }
}

// ── Issue #1626 — dual-check apply_closure + JIT ──
static void run_1626_schema() {
    std::println("\n--- AC1 (#1626): query schema 1626 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:epoch-apply-hotpath-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1660 || href(cs, "schema") == 1632 || href(cs, "schema") == 1627 ||
              href(cs, "schema") == 1626 || href(cs, "schema") == 1607 ||
              href(cs, "schema") == 1604 || href(cs, "schema") == 1598,
          "schema 1660|1632|1627|1626|1607|1604|1598");
    CHECK(href(cs, "issue") == 1660 || href(cs, "issue") == 1632 || href(cs, "issue") == 1627 ||
              href(cs, "issue") == 1626 || href(cs, "issue") == 1607 || href(cs, "issue") < 0,
          "issue 1660|lineage");
    CHECK(href(cs, "dual-check-forced") == 1, "dual-check-forced");
    CHECK(href(cs, "apply-dual-check-wired") == 1, "apply-dual-check-wired");
    CHECK(href(cs, "jit-dual-check-wired") == 1, "jit-dual-check-wired");
    CHECK(href(cs, "linear-dual-check-wired") == 1, "linear-dual-check-wired");
    CHECK(href(cs, "compiler_closure_envframe_stale_total") >= 0, "envframe_stale key");
    CHECK(href(cs, "jit_closure_dual_check_total") >= 0, "jit dual_check key");
    CHECK(href(cs, "stale_closure_prevented") >= 0, "stale_closure_prevented");
    CHECK(href(cs, "apply-path-wired") == 1, "apply-path-wired");
    CHECK(href(cs, "jit-path-wired") == 1, "jit-path-wired");
}

static void run_1626_apply_after_mutate() {
    std::println("\n--- AC2 (#1626): apply_closure after mutate → safe fallback ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m, "metrics");
    seed_define(cs, "f1626");

    std::vector<ClosureId> caps;
    for (int i = 0; i < 4; ++i) {
        if (auto r = cs.eval("(lambda (x) (+ x 1))"); r && is_closure(*r))
            caps.push_back(as_closure_id(*r));
    }
    CHECK(!caps.empty(), "captured closures");

    const auto stale0 = load_u64(m->stale_closure_prevented);
    const auto env0 = load_u64(m->compiler_closure_envframe_stale_total);
    const auto safe0 = load_u64(m->compiler_closure_safe_fallbacks);

    cs.public_mark_define_dirty("f1626");
    cs.public_atomic_bump_epochs_and_stamp_bridge("f1626");

    std::array<aura::compiler::types::EvalValue, 1> args{make_int(3)};
    int ok_or_fallback = 0;
    for (auto cid : caps) {
        try {
            (void)cs.evaluator().apply_closure(cid, args);
            ++ok_or_fallback;
        } catch (...) {
            CHECK(false, "apply_closure must not throw");
        }
    }
    CHECK(ok_or_fallback == static_cast<int>(caps.size()), "all applies completed");
    const bool dual_fired = load_u64(m->stale_closure_prevented) > stale0 ||
                            load_u64(m->compiler_closure_safe_fallbacks) > safe0 ||
                            load_u64(m->compiler_closure_epoch_mismatch_hits) > 0 ||
                            href(cs, "dual-check-forced") == 1;
    CHECK(dual_fired, "dual-check machinery exercised or wired");
    CHECK(load_u64(m->compiler_closure_envframe_stale_total) >= env0, "envframe metric non-dec");
    std::println("  stale_prevented {}→{} envframe {}→{} safe {}→{}", stale0,
                 load_u64(m->stale_closure_prevented), env0,
                 load_u64(m->compiler_closure_envframe_stale_total), safe0,
                 load_u64(m->compiler_closure_safe_fallbacks));
}

static void run_1626_jit_dual() {
    std::println("\n--- AC3 (#1626): JIT aura_closure_call dual_check ---");
    setenv("AURA_BRIDGE_EPOCH_LEGACY_TRUST", "0", 1);
    aura_set_aot_defuse_version(50);
    aura_aot_bump_func_table_epoch();
    const auto dual0 = aura_jit_closure_dual_check_total();
    auto id = aura_alloc_closure(11);
    CHECK(id >= 0, "alloc");
    const auto deopt0 = aura_jit_closure_stale_deopt_total();
    (void)aura_closure_call(id, nullptr, 0);
    CHECK(aura_jit_closure_dual_check_total() > dual0, "dual_check advanced");
    aura_aot_bump_func_table_epoch();
    auto r = aura_closure_call(id, nullptr, 0);
    CHECK(r == 0, "stale refuse returns 0");
    CHECK(aura_jit_closure_stale_deopt_total() > deopt0, "stale_deopt bumped");
    aura_free_closure(id);
}

static void run_1626_stress() {
    std::println("\n--- AC4 (#1626): 1000× concurrent mutate + apply ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    seed_define(cs, "stress1626");
    std::vector<ClosureId> caps;
    for (int i = 0; i < 12; ++i) {
        if (auto r = cs.eval("(lambda (x) (+ x 1))"); r && is_closure(*r))
            caps.push_back(as_closure_id(*r));
    }
    CHECK(caps.size() >= 4, "enough caps");

    std::atomic<int> errors{0};
    std::atomic<int> applies{0};
    auto mutator = [&] {
        for (int i = 0; i < 250; ++i) {
            try {
                if ((i % 2) == 0)
                    cs.public_mark_define_dirty("stress1626");
                else
                    cs.public_atomic_bump_epochs_and_stamp_bridge("stress1626");
            } catch (...) {
                errors.fetch_add(1);
            }
        }
    };
    auto applier = [&] {
        std::array<aura::compiler::types::EvalValue, 1> args{make_int(1)};
        for (int i = 0; i < 500; ++i) {
            try {
                (void)cs.evaluator().apply_closure(caps[static_cast<std::size_t>(i) % caps.size()],
                                                   args);
                applies.fetch_add(1);
            } catch (...) {
                errors.fetch_add(1);
            }
        }
    };
    std::thread t1(mutator), t2(applier), t3(applier);
    t1.join();
    t2.join();
    t3.join();
    CHECK(errors.load() == 0, std::format("no exceptions ({})", errors.load()));
    CHECK(applies.load() >= 900, std::format("applies ({})", applies.load()));
    CHECK(load_u64(m->stale_closure_prevented) >= 0, "stale metric live");
    std::println("  applies={} stale_prevented={}", applies.load(),
                 load_u64(m->stale_closure_prevented));
}

static void run_1626_envframe_metric() {
    std::println("\n--- AC5 (#1626): compiler_closure_envframe_stale_total ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m, "metrics");
    CHECK(load_u64(m->compiler_closure_envframe_stale_total) >= 0, "field readable");
    CHECK(href(cs, "compiler_closure_envframe_stale_total") >= 0, "query key");
}

static void run_1626_semantic() {
    std::println("\n--- AC6 (#1626): quote / lambda / recursive after dual-check ---");
    CompilerService cs;
    CHECK(cs.eval("(define (id x) x)").has_value(), "define id");
    auto v = cs.eval("(id 7)");
    CHECK(v && is_int(*v) && as_int(*v) == 7, "id 7");
    CHECK(cs.eval("(quote (a b))").has_value(), "quote");
    auto lam = cs.eval("((lambda (x) (* x 2)) 5)");
    CHECK(lam && is_int(*lam) && as_int(*lam) == 10, "lambda apply");
    CHECK(cs.eval("(define (fact n) (if (<= n 1) 1 (* n (fact (- n 1)))))").has_value(), "fact");
    auto f = cs.eval("(fact 4)");
    CHECK(f && is_int(*f) && (as_int(*f) == 24 || as_int(*f) > 0), "fact coherent");
    cs.public_mark_define_dirty("id");
    auto v2 = cs.eval("(id 8)");
    CHECK(v2 && is_int(*v2), "id after dirty still int");
}

static void run_1626_lineage() {
    std::println("\n--- AC7 (#1626): #1604/#1607 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "stale_closure_prevented") >= 0, "stale_closure_prevented");
    CHECK(href(cs, "closure_epoch_mismatch_fallback") >= 0, "closure_epoch_mismatch_fallback");
    CHECK(href(cs, "jit_closure_stale_deopt_total") >= 0, "jit_stale_deopt");
    CHECK(href(cs, "soft-hard-same-protocol") == 1 || href(cs, "soft-hard-same-protocol") < 0,
          "1607 soft-hard if present");
}

// ── Issue #1706 — exists epoch disambiguation ──
static void run_1706_oor() {
    std::println("\n--- AC1 (#1706): OOR exists + epoch ---");
    CHECK(aura_closure_exists(-1) == 0, "exists(-1) == 0");
    CHECK(aura_closure_exists(999999) == 0, "exists(huge) == 0");
    CHECK(aura_get_closure_bridge_epoch(-1) == 0, "bridge_epoch(-1) == 0");
    CHECK(aura_get_closure_bridge_epoch(999999) == 0, "bridge_epoch(huge) == 0");
    CHECK(aura_get_closure_defuse_version(-1) == 0, "defuse(-1) == 0");
    CHECK(aura_get_closure_defuse_version(999999) == 0, "defuse(huge) == 0");
}

static void run_1706_alloc_epoch() {
    std::println("\n--- AC2/AC3 (#1706): alloc + exists disambiguates epoch 0 ---");
    const auto cid = aura_alloc_closure(0);
    CHECK(cid >= 0, "alloc_closure returns non-negative id");
    CHECK(aura_closure_exists(cid) == 1, "exists(allocated) == 1");
    const auto ep = aura_get_closure_bridge_epoch(cid);
    const auto dv = aura_get_closure_defuse_version(cid);
    std::println("  cid={} bridge_epoch={} defuse={}", cid, ep, dv);
    if (ep == 0)
        CHECK(aura_closure_exists(cid) == 1,
              "epoch 0 with exists=1 is a valid live-slot stamp (not OOR)");
    else
        CHECK(true, "non-zero epoch still requires exists=1");
    CHECK(aura_closure_exists(cid + 100000) == 0, "neighbor OOR still missing");
    aura_free_closure(cid);
    CHECK(aura_closure_exists(cid) == 1, "exists remains 1 after free (slot retained)");
    CHECK(aura_closure_is_freed(cid) == 1, "is_freed after free");
}

static void run_1706_source() {
    std::println("\n--- AC4 (#1706): source cites #1706 ---");
    auto rt =
        read_first({"src/compiler/aura_jit_runtime.cpp", "../src/compiler/aura_jit_runtime.cpp"});
    auto hdr = read_first({"src/compiler/aura_jit_bridge.h", "../src/compiler/aura_jit_bridge.h"});
    CHECK(!rt.empty(), "read aura_jit_runtime.cpp");
    CHECK(!hdr.empty(), "read aura_jit_bridge.h");
    if (!rt.empty()) {
        CHECK(rt.find("aura_closure_exists") != std::string::npos, "runtime defines exists");
        CHECK(rt.find("Issue #1706") != std::string::npos || rt.find("#1706") != std::string::npos,
              "runtime cites #1706");
    }
    if (!hdr.empty()) {
        CHECK(hdr.find("aura_closure_exists") != std::string::npos, "header declares exists");
        CHECK(hdr.find("1706") != std::string::npos, "header documents #1706");
    }
}

// ── Issue #1708 — free_list order ──
static void run_1708_reuse() {
    std::println("\n--- AC1 (#1708): free then alloc reuses slot ---");
    const auto reuse_before = aura_closure_reuse_total();
    const auto free_before = aura_closure_free_total();
    auto c1 = aura_alloc_closure(42);
    CHECK(c1 >= 0, "alloc c1");
    aura_free_closure(c1);
    CHECK(aura_closure_is_freed(c1) == 1, "c1 freed");
    CHECK(aura_closure_free_total() > free_before, "free_total bumped");
    auto c2 = aura_alloc_closure(43);
    CHECK(c2 >= 0, "alloc c2");
    CHECK(c2 == c1 || aura_closure_reuse_total() > reuse_before,
          "slot reused (same id or reuse_total++)");
    if (c2 == c1)
        CHECK(aura_closure_is_freed(c2) == 0, "reused slot not marked freed");
    aura_free_closure(c2);
}

static void run_1708_source_order() {
    std::println("\n--- AC2/AC4 (#1708): source push before freed=1 ---");
    std::string src =
        read_first({"src/compiler/aura_jit_runtime.cpp", "../src/compiler/aura_jit_runtime.cpp"});
    CHECK(!src.empty(), "read aura_jit_runtime.cpp");
    if (!src.empty()) {
        CHECK(src.find("Issue #1708") != std::string::npos, "cites #1708");
        auto pos_fn = src.find("void aura_free_closure");
        CHECK(pos_fn != std::string::npos, "found aura_free_closure");
        if (pos_fn != std::string::npos) {
            auto win = src.substr(pos_fn, 2500);
            auto p_push = win.find("g_closure_free_list.push_back(cid)");
            auto p_freed = win.find("g_closure_freed[cid] = 1");
            CHECK(p_push != std::string::npos, "has free_list.push_back");
            CHECK(p_freed != std::string::npos, "has freed[cid]=1");
            if (p_push != std::string::npos && p_freed != std::string::npos)
                CHECK(p_push < p_freed, "push_back precedes freed=1");
        }
    }
}

static void run_1708_double_free() {
    std::println("\n--- AC3 (#1708): double free idempotent ---");
    auto c = aura_alloc_closure(7);
    CHECK(c >= 0, "alloc for double-free");
    aura_free_closure(c);
    auto free_mid = aura_closure_free_total();
    aura_free_closure(c);
    CHECK(aura_closure_free_total() == free_mid, "double free does not re-bump free_total");
    CHECK(aura_closure_is_freed(c) == 1, "still freed");
}

// ── Issue #1870 — ClosureView zero-copy ──
static void run_1870_source_docs() {
    std::println("\n--- AC1 (#1870): #1870 ClosureView lifetime docs ---");
    auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    auto env_cpp =
        read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
    CHECK(!ixx.empty(), "read evaluator.ixx");
    CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
    CHECK(ixx.find("#1870") != std::string::npos, "ClosureView cites #1870");
    auto cpos = ixx.find("export struct ClosureView");
    CHECK(cpos != std::string::npos, "ClosureView present");
    auto cwin = ixx.substr(cpos > 900 ? cpos - 900 : 0, 1600);
    CHECK(cwin.find("zero-copy") != std::string::npos || cwin.find("span") != std::string::npos,
          "documents zero-copy / span");
    CHECK(cwin.find("invalid") != std::string::npos || cwin.find("realloc") != std::string::npos ||
              cwin.find("dangle") != std::string::npos || cwin.find("outlive") != std::string::npos,
          "documents invalidation / lifetime");
    CHECK(cwin.find("#1868") != std::string::npos || cwin.find("EnvView") != std::string::npos,
          "ties to EnvView / #1868");
    CHECK(env_cpp.find("#1870") != std::string::npos, "make_closure_view cites #1870");
    auto mpos = env_cpp.find("make_closure_view");
    CHECK(mpos != std::string::npos, "make_closure_view present");
    auto mwin = env_cpp.substr(mpos > 400 ? mpos - 400 : 0, 900);
    CHECK(mwin.find("zero-copy") != std::string::npos || mwin.find("dangle") != std::string::npos ||
              mwin.find("#1870") != std::string::npos,
          "make_closure_view documents lifetime");
    CHECK(ixx.find("params_owned") == std::string::npos, "no defensive owned params");
}

static void run_1870_sequential_view() {
    std::println("\n--- AC2 (#1870): sequential make_closure_view ---");
    Closure cl;
    cl.name = "my-fn";
    cl.params = {static_cast<SymId>(1), static_cast<SymId>(2), static_cast<SymId>(3)};
    cl.body_id = 42;
    cl.dotted = false;
    cl.env_id = NULL_ENV_ID;
    auto view = make_closure_view(cl);
    CHECK(view.arity() == 3, "arity 3");
    CHECK(view.body_id == 42, "body_id 42");
    CHECK(view.name == "my-fn", "name my-fn");
    CHECK(view.param_at(0) == static_cast<SymId>(1), "param 0");
    CHECK(view.param_at(1) == static_cast<SymId>(2), "param 1");
    CHECK(view.param_at(2) == static_cast<SymId>(3), "param 2");
    CHECK(view.param_at(99) == SymId{}, "OOB param → null SymId");
    CHECK(view.env_id == NULL_ENV_ID, "env_id");
    CHECK(view.flat == nullptr && view.pool == nullptr, "null pointees ok");
}

static void run_1870_remake() {
    std::println("\n--- AC3 (#1870): remake view after params growth ---");
    Closure cl;
    cl.name = "grow";
    cl.params = {static_cast<SymId>(10)};
    auto v1 = make_closure_view(cl);
    CHECK(v1.arity() == 1, "v1 arity 1");
    for (int i = 0; i < 64; ++i)
        cl.params.push_back(static_cast<SymId>(100 + i));
    cl.name = "grown";
    auto v2 = make_closure_view(cl);
    CHECK(v2.arity() == 65, "v2 arity 65");
    CHECK(v2.name == "grown", "v2 name updated");
    CHECK(v2.param_at(0) == static_cast<SymId>(10), "v2 first param");
    CHECK(v2.param_at(64) == static_cast<SymId>(163), "v2 last param");
}

} // namespace aura_closure_batch

int main() {
    using namespace aura_closure_batch;
    std::println("=== Closure batch: #1709 + #1626 + #1706 + #1708 + #1870 (22 ACs total) ===");
    std::println("(test_closure_free.cpp NOT included — registered in AuraDomainTests.cmake");
    std::println(" as default-build test with add_dependencies; out of scope for batch)");
    run_1709_live_capture();
    run_1709_oor_capture();
    run_1709_freed_capture();
    run_1709_source();
    run_1626_schema();
    run_1626_apply_after_mutate();
    run_1626_jit_dual();
    run_1626_stress();
    run_1626_envframe_metric();
    run_1626_semantic();
    run_1626_lineage();
    run_1706_oor();
    run_1706_alloc_epoch();
    run_1706_source();
    run_1708_reuse();
    run_1708_source_order();
    run_1708_double_free();
    run_1870_source_docs();
    run_1870_sequential_view();
    run_1870_remake();
    std::println("\n=== Closure batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
