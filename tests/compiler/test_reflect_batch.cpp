// test_reflect_batch.cpp
// B pilot #15 (after soa in 09062244): consolidated reflect family
// — Issues #454 + #1611 + #1648 + #551 + #1679 (Reflection-to-EDSL bridge
// + MacroIntroduced hygiene + nested struct throw-site observability +
// post-mutation Guard impact snapshot + validate cycle guard) into one
// batch driver.
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention (per_defuse_batch /
// env_lookup_batch / fiber_resume_batch / compact_sweep_batch /
// incremental_relower_batch / macro_reflect_batch / incremental_type_batch /
// linear_ownership_batch / dead_coercion_batch / mutation_boundary_batch /
// walk_batch / compact_batch / gc_batch / soa_batch precedents): single
// binary with CHECK() + per-issue AC blocks in namespace
// aura_reflect_batch { run_NNN_xxx() }; EXCLUDE_FROM_ALL.
//
// AC map (consolidated, 30 ACs total):
//   Issue #454  — 8 ACs: query:reflect-edsl-bridge-stats +
//                  query:reflect-node-members on Define/Let + schema-of-marker
//                  + node-marker (SyntaxMarker) + query:marker-stats +
//                  reflect-type/members on record + mutate under Guard
//                  bridge stats monotonic + mutation-log:summary +
//                  reflect-postmutate-stats / query:node regression
//   Issue #1611 — 6 ACs: reflect.hh hygiene APIs +
//                  post_mutation MacroIntroduced check +
//                  query:reflect-postmutate-stats schema 1611 +
//                  reflect mutate on macro workspace hygiene +
//                  deserialize-hygiene-wired +
//                  allow-macro-mutate flag + checks/rejects counters +
//                  MutationReflectHealth enforce reject
//   Issue #1648 — 4 ACs: file-scope atomic counter in reflect.hh +
//                  auto_serialize throw site bump + auto_deserialize_struct
//                  throw site bump + #1676 follow-up reference
//                  (NOTE: design doc check dropped — docs/design/ removed
//                  per Anqi 2026-07-19 #1655 directive, commit a695c6b1)
//   Issue #551  — 9 ACs: 4 reflect/snapshot counters reachable + start at 0 +
//                  query:reflect-postmutate-stats hash +
//                  impact_snapshot_count bumps on Guard dtor success +
//                  200-iter cycle + schema_validation setters observable +
//                  dirty_nodes_in_snapshot setter/getter round-trip +
//                  8-thread concurrent typed mutate no crash +
//                  (gc-heap) + reflect-snapshot integration +
//                  regression on existing query primitives
//   Issue #1679 — 4 ACs: 2-node A↔B cycle terminates +
//                  self-loop node terminates + acyclic tree still validates +
//                  wall time for cycle << hang threshold (1s)

#include "test_harness.hpp"
#include "reflect/hygiene_validate.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace aura_reflect_batch {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static bool contains(const std::string& s, std::string_view needle) noexcept {
    return s.find(needle) != std::string::npos;
}

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── Issue #454 — Reflection-to-EDSL bridge closed loop ──
static std::int64_t bridge_stats_454(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:reflect-edsl-bridge-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void run_454_matrix() {
    std::println("\n--- Issue #454: Reflection-to-EDSL bridge closed loop ---");
    CompilerService cs;
    std::println("\n--- AC1 (#454): query:reflect-edsl-bridge-stats ---");
    const auto s0 = bridge_stats_454(cs);
    std::println("  reflect-edsl-bridge-stats = {}", s0);
    CHECK(s0 >= 0, "bridge-stats non-negative");

    std::println("\n--- AC2 (#454): query:reflect-node-members ---");
    cs.eval("(set-code \"(define x 42) (let ((y 1)) (+ y x))\")");
    CHECK(cs.eval("(eval-current)").has_value(), "workspace eval");
    auto defs = cs.eval("(stats:get \"ast:defs\")");
    CHECK(defs && is_pair(*defs), "ast:defs returns alist");
    auto node_r = cs.eval("(query:reflect-node-members 0)");
    CHECK(node_r && is_pair(*node_r), "reflect-node-members returns alist");
    auto marker_r = cs.eval("(query:node-marker 0)");
    CHECK(marker_r && is_string(*marker_r), "node-marker returns string");

    std::println("\n--- AC3 (#454): SyntaxMarker schema introspection ---");
    (void)cs.eval("(typecheck-current)");
    auto schema = cs.eval("(query:schema-of-marker \"User\")");
    CHECK(schema.has_value(), "schema-of-marker User reachable");
    auto macro_marker = cs.eval("(query:node-marker 0)");
    CHECK(macro_marker && is_string(*macro_marker), "node-marker on define");

    std::println("\n--- AC4 (#454): query:marker-stats ---");
    auto ms = cs.eval("(engine:metrics \"query:marker-stats\")");
    CHECK(ms && is_pair(*ms), "marker-stats returns list");
    auto total = cs.eval("(car (cdr (cdr (cdr (engine:metrics \"query:marker-stats\")))))");
    CHECK(total && is_int(*total) && as_int(*total) > 0, "marker-stats total > 0");

    std::println("\n--- AC5 (#454): reflect-type + reflect-members ---");
    auto rt = cs.eval("(reflect-type \"Int\")");
    CHECK(rt && is_pair(*rt), "reflect-type returns structured list for Int");
    auto rm = cs.eval("(reflect-members \"Int\")");
    CHECK(rm.has_value(), "reflect-members reachable");

    std::println("\n--- AC6 (#454): mutate + bridge stats monotonic ---");
    cs.eval("(set-code \"(define acc 0)\")");
    CHECK(cs.eval("(eval-current)").has_value(), "mutate workspace setup");
    const auto stats6a = bridge_stats_454(cs);
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    (void)cs.eval("(query:pattern \"acc\")");
    const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
    for (int i = 0; i < 3; ++i) {
        CHECK(cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(10 + i) + "\")").has_value(),
              "mutate:rebind ok");
        (void)cs.eval("(eval-current)");
    }
    const auto stats6b = bridge_stats_454(cs);
    std::println("  bridge-stats: {} -> {} hygiene_skips: {} -> {}", stats6a, stats6b, skips0,
                 skips1);
    CHECK(stats6b >= stats6a, "bridge-stats monotonic after mutate");
    CHECK(skips1 >= skips0, "marker introspection skips monotonic");

    std::println("\n--- AC7 (#454): mutation-log:summary ---");
    auto mls = cs.eval("(mutation-log:summary)");
    CHECK(mls.has_value(), "mutation-log:summary reachable after mutate");

    std::println("\n--- AC8 (#454): query regression ---");
    auto rps = cs.eval("(engine:metrics \"query:reflect-postmutate-stats\")");
    auto qn = cs.eval("(query:node 0)");
    CHECK(rps && is_hash(*rps), "reflect-postmutate-stats regression");
    CHECK(qn && is_pair(*qn), "query:node regression");
}

// ── Issue #1611 — reflect.hh MacroIntroduced hygiene ──
static bool setup_macro_ws_1611(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(d 1) (d 2) (d 3) "
                 "(define base 10) (+ base 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_1611_hh_api() {
    std::println("\n--- AC1 (#1611): hygiene_validate.hh / reflect hygiene APIs ---");
    std::string err;
    CHECK(aura::reflect::hygiene_allows_evolution(aura::reflect::HygieneMarker::User, false, &err),
          "User marker validates without allow");
    err.clear();
    CHECK(!aura::reflect::hygiene_allows_evolution(aura::reflect::HygieneMarker::MacroIntroduced,
                                                   false, &err),
          "MacroIntroduced without allow rejects");
    CHECK(!err.empty(), "typed error string on reject");
    err.clear();
    CHECK(aura::reflect::hygiene_allows_evolution(aura::reflect::HygieneMarker::MacroIntroduced,
                                                  true, &err),
          "MacroIntroduced with allow passes");
    CHECK(!aura::reflect::validate_deserialize_hygiene(
              aura::reflect::HygieneMarker::MacroIntroduced, false, &err),
          "deserialize rejects MacroIntroduced without allow");
    CHECK(aura::reflect::validate_deserialize_hygiene(aura::reflect::HygieneMarker::MacroIntroduced,
                                                      true, &err),
          "deserialize allows MacroIntroduced with allow");
}

static void run_1611_stats_schema() {
    std::println("\n--- AC2/AC3 (#1611): reflect-postmutate-stats schema 1611 ---");
    CompilerService cs;
    CHECK(setup_macro_ws_1611(cs), "macro workspace");
    CHECK(cs.eval("(mutate:rebind \"base\" \"42\")").has_value(), "mutate:rebind user binding");
    auto h = cs.eval("(engine:metrics \"query:reflect-postmutate-stats\")");
    CHECK(h && is_hash(*h), "reflect-postmutate-stats hash");
    CHECK(href(cs, "query:reflect-postmutate-stats", "schema") == 1611, "schema 1611");
    CHECK(href(cs, "query:reflect-postmutate-stats", "issue") == 1611, "issue 1611");
    CHECK(href(cs, "query:reflect-postmutate-stats", "hygiene-aware-validate-wired") == 1,
          "hygiene-aware-validate-wired");
    CHECK(href(cs, "query:reflect-postmutate-stats", "post-mutation-macro-check-wired") == 1,
          "post-mutation-macro-check-wired");
    CHECK(href(cs, "query:reflect-postmutate-stats", "deserialize-hygiene-wired") == 1,
          "deserialize-hygiene-wired");
    CHECK(href(cs, "query:reflect-postmutate-stats", "reflect-macro-hygiene-checks") >= 0,
          "reflect-macro-hygiene-checks");
    CHECK(href(cs, "query:reflect-postmutate-stats", "reflect-macro-hygiene-rejects") >= 0,
          "reflect-macro-hygiene-rejects");
    CHECK(href(cs, "query:reflect-postmutate-stats", "allow-macro-mutate") == 0 ||
              href(cs, "query:reflect-postmutate-stats", "allow-macro-mutate") == 1,
          "allow-macro-mutate readable");
}

static void run_1611_mutate_hygiene() {
    std::println("\n--- AC4 (#1611): mutate on macro workspace hygiene holds ---");
    CompilerService cs;
    CHECK(setup_macro_ws_1611(cs), "macro workspace");
    auto r = cs.eval("(mutate:rebind \"base\" \"99\")");
    CHECK(r.has_value(), "user rebind ok");
    CHECK(href(cs, "query:reflect-postmutate-stats", "schema") == 1611, "schema after mutate");
    auto ok = cs.eval("(+ 1 1)");
    CHECK(ok.has_value(), "eval ok after cycle");
}

static void run_1611_allow_flag() {
    std::println("\n--- AC6 (#1611): allow flag + reflect:validate-macro-body ---");
    CompilerService cs;
    CHECK(setup_macro_ws_1611(cs), "macro workspace");
    auto allow0 = cs.eval("(hygiene:allow-macro-mutate?)");
    CHECK(allow0 && is_bool(*allow0), "hygiene:allow-macro-mutate? reachable");
    auto macro_list = cs.eval("(query:macro-introduced)");
    if (macro_list && !is_int(*macro_list)) {
        auto len = cs.eval("(length (query:macro-introduced))");
        if (len && is_int(*len) && as_int(*len) > 0) {
            auto first = cs.eval("(car (query:macro-introduced))");
            if (first && is_int(*first)) {
                auto v = cs.eval(std::format("(reflect:validate-macro-body {})", as_int(*first)));
                CHECK(v.has_value(), "reflect:validate-macro-body returns");
                CHECK(href(cs, "query:reflect-postmutate-stats", "reflect-macro-hygiene-checks") >=
                          0,
                      "checks after validate-macro-body");
            }
        }
    }
    CHECK(cs.eval("(hygiene:set-allow-macro-mutate! #t)").has_value(), "set allow #t");
    CHECK(href(cs, "query:reflect-postmutate-stats", "allow-macro-mutate") == 1,
          "allow-macro-mutate flag reflected in stats");
    CHECK(cs.eval("(hygiene:set-allow-macro-mutate! #f)").has_value(), "set allow #f");
}

static void run_1611_health_struct() {
    std::println("\n--- #1611: MutationReflectHealth enforce reject ---");
    aura::reflect::MutationReflectHealth h;
    h.marker_consistent = true;
    h.generation_healthy = true;
    h.dirty_macro_nodes = 3;
    h.allow_macro_evolution = false;
    h.enforce_macro_hygiene_reject = true;
    std::string err;
    CHECK(!aura::reflect::validate_mutation_reflect_health(h, &err),
          "enforce reject without allow");
    h.allow_macro_evolution = true;
    CHECK(aura::reflect::validate_mutation_reflect_health(h, &err), "enforce allow passes");
}

// ── Issue #1648 — nested struct throw-site observability ──
static void run_1648_filescope_counter() {
    std::println("\n--- AC1 (#1648): file-scope atomic counter ---");
    std::string rh;
    for (const char* p : {"src/reflect/reflect.hh", "../src/reflect/reflect.hh"}) {
        rh = read_file(p);
        if (!rh.empty())
            break;
    }
    CHECK(!rh.empty(), "read reflect.hh");
    bool counter_decl = contains(rh, "reflect_nested_struct_throw_count_ref()") &&
                        contains(rh, "std::atomic<std::uint64_t>");
    bool accessor_read = contains(rh, "aura_reflect_nested_struct_throw_count_v_read()");
    bool accessor_bump = contains(rh, "aura_reflect_nested_struct_throw_count_v_bump(");
    CHECK(counter_decl && accessor_read && accessor_bump,
          "file-scope atomic + C-linkage accessor present");
}

static void run_1648_throw_site_serialize() {
    std::println("\n--- AC1 (#1648): auto_serialize throw site wired ---");
    std::string rh;
    for (const char* p : {"src/reflect/reflect.hh", "../src/reflect/reflect.hh"}) {
        rh = read_file(p);
        if (!rh.empty())
            break;
    }
    CHECK(!rh.empty(), "read reflect.hh");
    bool serialized_bump =
        contains(rh, "auto_serialize: nested MemberKind::Struct not yet supported") &&
        contains(rh, "aura_reflect_nested_struct_throw_count_v_bump(1);");
    CHECK(serialized_bump, "auto_serialize throw site wired with counter bump");
}

static void run_1648_throw_site_deserialize() {
    std::println("\n--- AC1 (#1648): auto_deserialize_struct throw site wired ---");
    std::string rh;
    for (const char* p : {"src/reflect/reflect.hh", "../src/reflect/reflect.hh"}) {
        rh = read_file(p);
        if (!rh.empty())
            break;
    }
    CHECK(!rh.empty(), "read reflect.hh");
    bool deserialized_bump =
        contains(rh, "auto_deserialize_struct: nested MemberKind::Struct not yet supported") &&
        contains(rh, "aura_reflect_nested_struct_throw_count_v_bump(1);");
    CHECK(deserialized_bump, "auto_deserialize_struct throw site wired with counter bump");
}

static void run_1648_followup_pointer() {
    std::println("\n--- #1648 → #1676 follow-up reference ---");
    std::string rh;
    for (const char* p : {"src/reflect/reflect.hh", "../src/reflect/reflect.hh"}) {
        rh = read_file(p);
        if (!rh.empty())
            break;
    }
    CHECK(!rh.empty() && contains(rh, "#1676"),
          "#1676 follow-up referenced in throw-site comments");
}

// ── Issue #551 — post-mutation Guard impact snapshot ──
static int k_long_iters_551() {
    return k_int_env("AURA_STRESS_ITERS", 200);
}

static void run_551_counters_reachable() {
    std::println("\n--- AC1 (#551): 4 reflect/snapshot counters reachable ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto s0 = cs.evaluator().get_impact_snapshot_count();
    const auto p0 = cs.evaluator().get_schema_validation_pass_count();
    const auto f0 = cs.evaluator().get_schema_validation_fail_count();
    const auto d0 = cs.evaluator().get_dirty_nodes_in_snapshot();
    std::println("  baseline: impact_snapshots={} schema_pass={} schema_fail={} dirty_nodes={}", s0,
                 p0, f0, d0);
    CHECK(s0 == 0, "impact_snapshot_count starts at 0");
    CHECK(p0 == 0, "schema_validation_pass_count starts at 0");
    CHECK(f0 == 0, "schema_validation_fail_count starts at 0");
    CHECK(d0 == 0, "dirty_nodes_in_snapshot starts at 0");
}

static void run_551_query_postmutate_stats() {
    std::println("\n--- AC2 (#551): query:reflect-postmutate-stats returns hash ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(engine:metrics \"query:reflect-postmutate-stats\")");
    CHECK(r.has_value(), "(engine:metrics \"query:reflect-postmutate-stats\") returns");
    CHECK(is_hash(*r), "(engine:metrics \"query:reflect-postmutate-stats\") is hash");
}

static void run_551_impact_snapshot_under_mutate() {
    std::println("\n--- AC3 (#551): impact_snapshot_count bumps on Guard dtor success ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    const auto s0 = cs.evaluator().get_impact_snapshot_count();
    for (int i = 0; i < 5; ++i) {
        (void)cs.eval("(mutate:replace-value (define a " + std::to_string(i) + ") (define a " +
                      std::to_string(i) + "))");
    }
    const auto s1 = cs.evaluator().get_impact_snapshot_count();
    std::println("  impact_snapshot: {} -> {} (delta {})", s0, s1, s1 - s0);
    CHECK(s1 > s0, "impact_snapshot_count bumped after Aura mutate (Guard dtor success path)");
}

static void run_551_long_running_cycle() {
    std::println("\n--- AC4 (#551): {} iters reflect snapshot cycle ---", k_long_iters_551());
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    const auto s0 = cs.evaluator().get_impact_snapshot_count();
    std::mt19937 rng(551u);
    std::uniform_int_distribution<int> val_dist(0, 999);
    for (int i = 0; i < k_long_iters_551(); ++i) {
        std::string code = std::string("(mutate:replace-value (define ") + (i & 1 ? "a" : "b") +
                           " " + std::to_string(val_dist(rng)) + ") (define " +
                           (i & 1 ? "a" : "b") + " " + std::to_string(val_dist(rng)) + "))";
        (void)cs.eval(code);
    }
    const auto s1 = cs.evaluator().get_impact_snapshot_count();
    std::println("  impact_snapshot: {} -> {} (delta {})", s0, s1, s1 - s0);
    CHECK(s1 >= s0 + static_cast<std::uint64_t>(k_long_iters_551() - 5),
          "impact_snapshot_count grew under mutate cycle");
}

static void run_551_schema_setters() {
    std::println("\n--- AC5 (#551): schema_validation_pass/fail setters observable ---");
    Evaluator ev;
    const auto p0 = ev.get_schema_validation_pass_count();
    const auto f0 = ev.get_schema_validation_fail_count();
    CHECK(p0 == 0, "schema_pass starts at 0");
    CHECK(f0 == 0, "schema_fail starts at 0");
    ev.bump_schema_validation_pass_count();
    ev.bump_schema_validation_pass_count();
    ev.bump_schema_validation_fail_count();
    const auto p1 = ev.get_schema_validation_pass_count();
    const auto f1 = ev.get_schema_validation_fail_count();
    std::println("  schema_pass: {} -> {} schema_fail: {} -> {}", p0, p1, f0, f1);
    CHECK(p1 == p0 + 2, "schema_pass bumped by 2");
    CHECK(f1 == f0 + 1, "schema_fail bumped by 1");
    const auto s = ev.get_impact_snapshot_count();
    std::println("  impact_snapshot_count: {}", s);
    CHECK(s >= 0, "impact_snapshot_count observable");
}

static void run_551_dirty_nodes_roundtrip() {
    std::println("\n--- AC6 (#551): dirty_nodes_in_snapshot setter + getter round-trip ---");
    Evaluator ev;
    const auto d0 = ev.get_dirty_nodes_in_snapshot();
    CHECK(d0 == 0, "dirty_nodes_in_snapshot starts at 0");
    ev.set_dirty_nodes_in_snapshot(100);
    CHECK(ev.get_dirty_nodes_in_snapshot() == 100,
          "dirty_nodes_in_snapshot set/get round-trip (100)");
    ev.set_dirty_nodes_in_snapshot(0);
    CHECK(ev.get_dirty_nodes_in_snapshot() == 0, "dirty_nodes_in_snapshot reset to 0");
}

static void run_551_eight_thread_concurrent() {
    std::println("\n--- AC7 (#551): 8 threads × 20 iters concurrent reflect mutate ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 0) (define b 0)\")");
    (void)cs.eval("(eval-current)");
    constexpr int n_threads = 8;
    constexpr int n_iters = 20;
    std::mutex mtx;
    std::atomic<int> completed{0};
    auto worker = [&](int tid) {
        for (int i = 0; i < n_iters; ++i) {
            std::lock_guard<std::mutex> lk(mtx);
            std::string code = "(mutate:replace-value (define v" + std::to_string(tid) + " " +
                               std::to_string(i) + ") (define v" + std::to_string(tid) + " " +
                               std::to_string(i) + "))";
            (void)cs.eval(code);
            completed.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < n_threads; ++i)
        threads.emplace_back(worker, i);
    for (auto& t : threads)
        t.join();

    const auto s = cs.evaluator().get_impact_snapshot_count();
    std::println("  completed: {}/{} impact_snapshot: {}", completed.load(), n_threads * n_iters,
                 s);
    CHECK(completed.load() == n_threads * n_iters,
          "all 160 ops completed (no crash under concurrent reflect mutate)");
    CHECK(s >= static_cast<std::uint64_t>(n_threads * n_iters),
          "impact_snapshot_count >= concurrent mutate count");
}

static void run_551_gc_heap() {
    std::println("\n--- AC8 (#551): (gc-heap) + reflect-snapshot integration ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2)\")");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(mutate:replace-value (define a 99) (define a 99))");
    auto r = cs.eval("(gc-heap)");
    CHECK(r.has_value(), "(gc-heap) callable after reflect mutate");
}

static void run_551_regression() {
    std::println("\n--- AC9 (#551): regression — existing primitives ---");
    CompilerService cs;
    auto r1 = cs.eval("(engine:metrics \"query:reflect-postmutate-stats\")");
    CHECK(r1.has_value() && is_hash(*r1),
          "(engine:metrics \"query:reflect-postmutate-stats\") (hash for #502)");
    auto r2 = cs.eval("(engine:metrics \"query:typed-mutation-stats\")");
    CHECK(r2.has_value() && is_int(*r2),
          "(engine:metrics \"query:typed-mutation-stats\") (regression for #550)");
    auto r3 = cs.eval("(query:dirty-impact)");
    CHECK(r3.has_value() && is_int(*r3), "(query:dirty-impact) (regression for #550)");
    auto r4 = cs.eval("(engine:metrics \"query:self-evolution-stability-stats\")");
    CHECK(r4.has_value() && is_int(*r4),
          "(engine:metrics \"query:self-evolution-stability-stats\") (regression for #549)");
    auto r5 = cs.eval("(engine:metrics \"query:panic-checkpoint-lifecycle-stats\")");
    CHECK(r5.has_value() && is_int(*r5),
          "(engine:metrics \"query:panic-checkpoint-lifecycle-stats\") (regression for #548)");
    CHECK(cs.eval("(define reg-551-a 10)").has_value(), "define (regression)");
    auto r7 = cs.eval("(+ reg-551-a reg-551-b)");
    CHECK(r7.has_value() && is_int(*r7) && as_int(*r7) == 42,
          "(+ reg-551-a reg-551-b) == 42 (regression)");
}

// ── Issue #1679 — runtime_reflect_validate_ast_subtree cycle guard ──
using clock_1679 = std::chrono::steady_clock;

static bool validate_terminates_1679(CompilerService& cs, aura::ast::NodeId nid,
                                     const char* label) {
    const auto t0 = clock_1679::now();
    auto r = cs.eval(std::format("(reflect:validate-macro-body {})", static_cast<int>(nid)));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock_1679::now() - t0).count();
    CHECK(r.has_value() && is_bool(*r), std::format("{} returns bool", label));
    CHECK(ms < 1000, std::format("{} finished in {}ms (< 1000ms hang threshold)", label, ms));
    std::println("  {} → {} in {}ms", label, as_bool(*r) ? "#t" : "#f", ms);
    return r.has_value() && is_bool(*r) && ms < 1000;
}

static void run_1679_acyclic_control() {
    std::println("\n--- AC3 (#1679): acyclic Begin tree ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define seed 1)\")").has_value(), "set-code seed");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto* flat = cs.workspace_flat();
    CHECK(flat != nullptr, "workspace_flat");
    auto lit = flat->add_literal(42);
    auto root = flat->add_begin(std::span<const aura::ast::NodeId>(&lit, 1));
    CHECK(root != aura::ast::NULL_NODE, "acyclic begin");
    CHECK(validate_terminates_1679(cs, root, "acyclic"), "acyclic validate ok");
}

static void run_1679_two_node_cycle() {
    std::println("\n--- AC1 (#1679): two-node A↔B cycle ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define seed 1)\")").has_value(), "set-code seed");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto* flat = cs.workspace_flat();
    CHECK(flat != nullptr, "workspace_flat");
    auto a = flat->add_node(aura::ast::NodeTag::Begin);
    auto b = flat->add_node(aura::ast::NodeTag::Begin);
    CHECK(a != aura::ast::NULL_NODE && b != aura::ast::NULL_NODE, "cycle nodes allocated");
    flat->insert_child(a, 0, b);
    flat->insert_child(b, 0, a);
    CHECK(validate_terminates_1679(cs, a, "cycle A↔B from a"), "cycle from a terminates");
    CHECK(validate_terminates_1679(cs, b, "cycle A↔B from b"), "cycle from b terminates");
}

static void run_1679_self_loop() {
    std::println("\n--- AC2 (#1679): self-loop ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define seed 1)\")").has_value(), "set-code seed");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto* flat = cs.workspace_flat();
    CHECK(flat != nullptr, "workspace_flat");
    auto s = flat->add_node(aura::ast::NodeTag::Begin);
    flat->insert_child(s, 0, s);
    CHECK(validate_terminates_1679(cs, s, "self-loop"), "self-loop terminates");
}

static void run_1679_hang_threshold() {
    std::println("\n--- AC4 (#1679): hang threshold embedded in AC1-AC3 ---");
    CHECK(true, "wall-time bound enforced");
}

} // namespace aura_reflect_batch

int main() {
    using namespace aura_reflect_batch;
    std::println("=== Reflect batch: #454 + #1611 + #1648 + #551 + #1679 (30 ACs total) ===");
    std::println("(NOTE: #1648 design doc check dropped — docs/design/ removed per Anqi");
    std::println(" 2026-07-19 #1655 directive, commit a695c6b1)");
    run_454_matrix();
    run_1611_hh_api();
    run_1611_stats_schema();
    run_1611_mutate_hygiene();
    run_1611_allow_flag();
    run_1611_health_struct();
    run_1648_filescope_counter();
    run_1648_throw_site_serialize();
    run_1648_throw_site_deserialize();
    run_1648_followup_pointer();
    run_551_counters_reachable();
    run_551_query_postmutate_stats();
    run_551_impact_snapshot_under_mutate();
    run_551_long_running_cycle();
    run_551_schema_setters();
    run_551_dirty_nodes_roundtrip();
    run_551_eight_thread_concurrent();
    run_551_gc_heap();
    run_551_regression();
    run_1679_acyclic_control();
    run_1679_two_node_cycle();
    run_1679_self_loop();
    run_1679_hang_threshold();
    std::println("\n=== Reflect batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
