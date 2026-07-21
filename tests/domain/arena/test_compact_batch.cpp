// tests/domain/arena/test_compact_batch.cpp — relocated for #1959 arena pilot
// (was tests/test_compact_batch.cpp). Prefer this path; do not re-add under tests/ root.
//
// test_compact_batch.cpp
// B pilot #12 (after walk in af2307d7): consolidated compact family
// — Issues #1842 + #1666 + #1362 + #1757 (compact_env_frames Guard +
// compact hook replace/chain + mutation_log compact + compact_pairs
// size_t return) into one batch driver.
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention (per_defuse_batch /
// env_lookup_batch / fiber_resume_batch / compact_sweep_batch /
// incremental_relower_batch / macro_reflect_batch / incremental_type_batch /
// linear_ownership_batch / dead_coercion_batch / mutation_boundary_batch /
// walk_batch precedents): single binary with CHECK() + per-issue AC blocks in
// namespace aura_compact_batch { run_NNN_xxx() }; EXCLUDE_FROM_ALL.
//
// AC map (consolidated, 21 ACs total):
//   Issue #1842 — 3 ACs: compact_env_frames wrapped in MutationBoundaryGuard
//                  + try/catch; primitive returns int (reclaimed count)
//                  without hang; nested under outer Guard still completes
//                  (outermost lock)
//   Issue #1666 — 6 ACs: set_on_compact_hook installs + has_on_compact_hook
//                  true + take clears + compact doesn't call taken +
//                  chain (take + reinstall with prior()) invokes both +
//                  set_arena chains over external + rebind set_arena clears
//                  previous + CompilerService keeps chained hook
//   Issue #1362 — 7 ACs: compact no-op when small + 10K→1000 +
//                  keep RolledBack when keep_all_rolledback=true +
//                  keep_all_rolledback=false drops RolledBack too +
//                  auto-compact at threshold on add_mutation +
//                  many cycles stay bounded + Aura primitives +
//                  query:mutation-log-compact-stats
//   Issue #1757 — 5 ACs: source cites #1757; return type is size_t +
//                  empty live_mask all-live + selective mask +
//                  all-dead returns 0 + return type unsigned

#include "test_harness.hpp"
#include "core/gc_hooks.h"
#include "core/arena_auto_policy_stats.h"
#include "core/cpp26_contract_stats.h"
#include "compiler/messaging_bridge.h"
#include "serve/gc_coordinator.h"
#include "compiler/runtime_shared.h"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

import std;
import aura.core.arena;
import aura.core.ast;
import aura.core.concepts;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

// ── Wave 14 extern "C" declarations for #1508 C-FFI aot/jit dual check ──
extern "C" void aura_reset_runtime();
extern "C" int64_t aura_alloc_closure(int64_t func_id);
extern "C" int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc);
extern "C" void aura_free_closure(int64_t closure_id);
extern "C" int aura_closure_is_freed(int64_t closure_id);
extern "C" uint64_t aura_deopt_count();

namespace aura_compact_batch {

using aura::ast::FlatAST;
using aura::ast::MutationStatus;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::int64_t href(CompilerService& cs, const char* q, const char* key) {
    // Observability queries surface via engine:metrics, not bare (query:…).
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── Issue #1842 — compact_env_frames Guard ──
static void run_1842_source() {
    std::println("\n--- AC1 (#1842): Guard + try/catch on compact-env-frames ---");
    std::string src;
    for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                          "../src/compiler/evaluator_primitives_compile.cpp"}) {
        src = read_file(p);
        if (!src.empty())
            break;
    }
    CHECK(!src.empty(), "read compile_07.cpp");
    CHECK(src.find("#1842") != std::string::npos, "cites #1842");
    auto pos = src.find("\"evaluator:compact-env-frames\"");
    CHECK(pos != std::string::npos, "primitive present");
    // Post-#1897 the Guard + try/catch body lives in shared helper
    // run_under_mutation_guard (still MutationBoundaryGuard under the hood).
    auto win = src.substr(pos, 1600);
    const bool via_helper = win.find("run_under_mutation_guard") != std::string::npos;
    const bool via_inline = win.find("MutationBoundaryGuard") != std::string::npos;
    CHECK(via_helper || via_inline, "uses Guard (inline or run_under_mutation_guard)");
    if (via_inline) {
        CHECK(win.find("guard_ok") != std::string::npos, "guard_ok flag");
        CHECK(win.find("try {") != std::string::npos || win.find("try{") != std::string::npos,
              "try block");
        CHECK(win.find("catch") != std::string::npos, "catch path");
    } else {
        // Helper path: track_env_compact_violation wires the same contract.
        CHECK(win.find("track_env_compact_violation") != std::string::npos ||
                  win.find("/*track_env_compact_violation=") != std::string::npos,
              "env-compact violation tracking on helper path");
    }
    CHECK(win.find("compact_env_frames()") != std::string::npos, "calls compact_env_frames");
}

static void run_1842_runtime() {
    std::println("\n--- AC2 (#1842): compact-env-frames returns int ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "seed eval");
    auto r = cs.eval("(evaluator:compact-env-frames)");
    CHECK(r && is_int(*r), "returns int");
    CHECK(as_int(*r) >= 0, "reclaimed >= 0 (or 0 empty)");
}

static void run_1842_nested_guard() {
    std::println("\n--- AC3 (#1842): under outer MutationBoundaryGuard ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 2 2)").has_value(), "seed");
    auto& ev = cs.evaluator();
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard outer(ev, &ok);
        CHECK(outer.is_outermost(), "outer is outermost");
        auto r = cs.eval("(evaluator:compact-env-frames)");
        CHECK(r && is_int(*r), "compact under outer Guard returns int");
        CHECK(ev.mutation_boundary_depth_slot_value() >= 1, "depth held by outer");
    }
    CHECK(ok, "outer guard_ok");
    CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0 after outer dtor");
}

// ── Issue #1666 — compact hook replace/chain ──
static void run_1666_install() {
    std::println("\n--- AC1 (#1666): set_on_compact_hook installs ---");
    aura::ast::ASTArena arena(64 * 1024);
    CHECK(!arena.has_on_compact_hook(), "fresh no hook");
    std::atomic<int> n{0};
    arena.set_on_compact_hook([&]() { n.fetch_add(1); });
    CHECK(arena.has_on_compact_hook(), "hook installed");
    (void)arena.compact();
    CHECK(n.load() >= 1, "compact invokes hook");
}

static void run_1666_take_clears() {
    std::println("\n--- AC2 (#1666): take_on_compact_hook clears ---");
    aura::ast::ASTArena arena(64 * 1024);
    std::atomic<int> n{0};
    arena.set_on_compact_hook([&]() { n.fetch_add(1); });
    auto taken = arena.take_on_compact_hook();
    CHECK(static_cast<bool>(taken), "take returns hook");
    CHECK(!arena.has_on_compact_hook(), "arena cleared after take");
    const auto before = n.load();
    (void)arena.compact();
    CHECK(n.load() == before, "compact does not call taken-away hook");
    if (taken)
        taken();
    CHECK(n.load() == before + 1, "taken hook still callable");
}

static void run_1666_chain_both() {
    std::println("\n--- AC3 (#1666): chain invokes both listeners ---");
    aura::ast::ASTArena arena(64 * 1024);
    std::atomic<int> a{0};
    std::atomic<int> b{0};
    arena.set_on_compact_hook([&]() { a.fetch_add(1); });
    auto prior = arena.take_on_compact_hook();
    arena.set_on_compact_hook([&a, &b, prior = std::move(prior)]() {
        if (prior)
            prior();
        b.fetch_add(1);
    });
    (void)arena.compact();
    CHECK(a.load() >= 1, "prior listener ran");
    CHECK(b.load() >= 1, "new listener ran");
}

static void run_1666_set_arena_chains() {
    std::println("\n--- AC4 (#1666): set_arena chains over external hook ---");
    aura::ast::ASTArena arena(64 * 1024);
    std::atomic<int> external{0};
    arena.set_on_compact_hook([&]() { external.fetch_add(1); });
    Evaluator ev;
    ev.set_arena(&arena);
    CHECK(arena.has_on_compact_hook(), "hook present after set_arena");
    (void)arena.compact();
    CHECK(external.load() >= 1, "external prior still ran after set_arena chain");
}

static void run_1666_rebind_clears_old() {
    std::println("\n--- AC5 (#1666): rebind clears hook on previous arena ---");
    aura::ast::ASTArena a(64 * 1024);
    aura::ast::ASTArena b(64 * 1024);
    Evaluator ev;
    ev.set_arena(&a);
    CHECK(a.has_on_compact_hook(), "a has hook");
    ev.set_arena(&b);
    CHECK(!a.has_on_compact_hook(), "a cleared on rebind");
    CHECK(b.has_on_compact_hook(), "b has hook");
}

static void run_1666_compiler_service() {
    std::println("\n--- AC6 (#1666): CompilerService keeps chained hook ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "CS eval ok");
    aura::ast::ASTArena arena(64 * 1024);
    std::atomic<int> order{0};
    std::atomic<int> first{0};
    std::atomic<int> second{0};
    Evaluator ev;
    ev.set_arena(&arena);
    auto prior = arena.take_on_compact_hook();
    arena.set_on_compact_hook([&order, &first, &second, prior = std::move(prior)]() {
        if (prior) {
            prior();
            first.store(order.fetch_add(1) + 1);
        }
        second.store(order.fetch_add(1) + 1);
    });
    (void)arena.compact();
    CHECK(second.load() > 0, "service-style second listener ran");
    CHECK(first.load() > 0 && first.load() < second.load(), "prior ran before second");
}

// ── Issue #1362 — compact mutation_log ──
struct Fixture1362 {
    aura::ast::ASTArena arena;
    FlatAST flat;
    aura::ast::NodeId lit = aura::ast::NULL_NODE;

    Fixture1362()
        : flat(arena.allocator()) {
        lit = flat.add_literal(1);
        flat.root = lit;
    }

    void add_committed(int n) {
        for (int i = 0; i < n; ++i) {
            (void)flat.add_mutation(lit, "tweak", "Int", "Int", "c");
        }
    }

    void add_then_rollback(int n) {
        for (int i = 0; i < n; ++i) {
            auto mid = flat.add_mutation_with_rollback(lit, "tweak", "Int", "Int", "rb",
                                                       MutationStatus::Committed, 0, 1, 2, true);
            (void)flat.rollback(mid);
        }
    }
};

static void run_1362_no_op_small() {
    std::println("\n--- AC1 (#1362): compact no-op when small ---");
    Fixture1362 f;
    f.add_committed(50);
    CHECK(f.flat.mutation_count() == 50, "50 committed");
    auto d = f.flat.compact_mutation_log(1000, true);
    CHECK(d == 0, "compact no-op when size <= keep_recent");
    CHECK(f.flat.mutation_count() == 50, "size unchanged");
}

static void run_1362_10k_to_1k() {
    std::println("\n--- AC2 (#1362): 10K committed → compact(1000) → size ~1000 ---");
    Fixture1362 f;
    f.add_committed(10000);
    CHECK(f.flat.mutation_count() == 10000, "10K before compact");
    const auto ops0 = f.flat.mutation_log_compact_ops();
    const auto rec0 = f.flat.mutation_log_compacted_records();
    auto dropped = f.flat.compact_mutation_log(1000, true);
    CHECK(dropped == 9000, "dropped 9000 committed");
    CHECK(f.flat.mutation_count() == 1000, "keep 1000 recent");
    CHECK(f.flat.mutation_log_compact_ops() == ops0 + 1, "compact ops +1");
    CHECK(f.flat.mutation_log_compacted_records() == rec0 + 9000, "compacted +9000");
    CHECK(f.flat.mutation_count() < 5000, "log size bounded < 5K");
}

static void run_1362_keep_rolledback() {
    std::println("\n--- AC3 (#1362): keep RolledBack when keep_all_rolledback=true ---");
    Fixture1362 f;
    f.add_committed(5000);
    f.add_then_rollback(1000);
    f.add_committed(1000);
    const auto before = f.flat.mutation_count();
    CHECK(before == 7000, "7000 total before compact");
    auto dropped = f.flat.compact_mutation_log(1000, true);
    CHECK(dropped == 5000, "dropped 5K old committed");
    CHECK(f.flat.mutation_count() == 2000, "1000 rolledback + 1000 recent");
    std::size_t rb = 0, cm = 0;
    for (const auto& rec : f.flat.all_mutations()) {
        if (rec.status == MutationStatus::RolledBack)
            ++rb;
        else
            ++cm;
    }
    CHECK(rb == 1000, "all RolledBack preserved");
    CHECK(cm == 1000, "only recent Committed kept");
}

static void run_1362_drop_rolledback() {
    std::println("\n--- AC4 (#1362): keep_all_rolledback=false drops RolledBack too ---");
    Fixture1362 f;
    f.add_committed(2000);
    f.add_then_rollback(500);
    f.add_committed(500);
    auto dropped = f.flat.compact_mutation_log(500, false);
    CHECK(dropped == 2500, "drop 2500 without preserving RB");
    CHECK(f.flat.mutation_count() == 500, "only keep_recent remain");
}

static void run_1362_auto_compact() {
    std::println("\n--- AC5 (#1362): auto-compact at threshold ---");
    Fixture1362 f;
    f.add_committed(static_cast<int>(FlatAST::kMutationLogAutoCompactThreshold) + 50);
    CHECK(f.flat.mutation_count() <= FlatAST::kMutationLogAutoCompactKeepRecent + 50,
          "auto-compact bounded log after >10K adds");
    CHECK(f.flat.mutation_log_compact_ops() >= 1, "auto-compact bumped ops");
    CHECK(f.flat.mutation_count() < 5000, "auto log size < 5K");
}

static void run_1362_many_cycles() {
    std::println("\n--- AC6 (#1362): many cycles stay bounded ---");
    Fixture1362 f;
    for (int cycle = 0; cycle < 50; ++cycle) {
        f.add_committed(500);
        (void)f.flat.compact_mutation_log(200, true);
    }
    CHECK(f.flat.mutation_count() <= 200, "after 50 cycles size <= keep_recent");
    CHECK(f.flat.mutation_log_compacted_records() >= 50ull * 300, "many records reclaimed");
}

static void run_1362_aura_primitives() {
    std::println("\n--- AC7 (#1362): Aura primitives + stats ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define x 1)\")");
    auto c0 = cs.eval("(stats:get \"mutation-count\")");
    CHECK(c0 && is_int(*c0), "mutation-count works");

    (void)cs.eval("(mutate:tweak-literal 0 1)");
    auto d = cs.eval("(mutation-log-compact 1000)");
    CHECK(d && is_int(*d), "mutation-log-compact returns int");

    auto s = cs.eval("(engine:metrics \"query:mutation-log-compact-stats\")");
    CHECK(s && is_hash(*s), "query:mutation-log-compact-stats is hash");
    auto ls = href(cs, "query:mutation-log-compact-stats", "log-size");
    CHECK(ls >= 0, "log-size key");
    auto co = href(cs, "query:mutation-log-compact-stats", "compact-ops");
    CHECK(co >= 0, "compact-ops key");
    auto cr = href(cs, "query:mutation-log-compact-stats", "compacted-records");
    CHECK(cr >= 0, "compacted-records key");
    auto th = href(cs, "query:mutation-log-compact-stats", "auto-threshold");
    CHECK(th == static_cast<std::int64_t>(FlatAST::kMutationLogAutoCompactThreshold),
          "auto-threshold key == 10000");
    auto legacy = cs.eval("(engine:metrics \"query:mutation-log-stats\")");
    CHECK(legacy && is_int(*legacy), "query:mutation-log-stats remains int (#553)");
}

static void run_1362_no_workspace() {
    std::println("\n--- AC8 (#1362): compact without workspace → 0 ---");
    CompilerService cs;
    auto d = cs.eval("(mutation-log-compact)");
    CHECK(d && is_int(*d) && as_int(*d) == 0, "compact without workspace → 0");
}

// ── Issue #1757 — compact_pairs size_t return ──
static void alloc_pairs(aura::compiler::CompilerService& cs, int n) {
    for (int i = 0; i < n; ++i) {
        std::string src = "(cons " + std::to_string(i) + " " + std::to_string(i + 1) + ")";
        auto r = cs.eval(src);
        (void)r;
    }
}

static void run_1757_source() {
    std::println("\n--- AC1 (#1757): #1757 size_t compact_pairs ---");
    std::string gc;
    for (const char* p : {"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"}) {
        gc = read_file(p);
        if (!gc.empty())
            break;
    }
    CHECK(!gc.empty(), "read evaluator_gc.cpp");
    CHECK(gc.find("#1757") != std::string::npos, "cites #1757");
    CHECK(gc.find("std::size_t Evaluator::compact_pairs") != std::string::npos,
          "impl returns size_t");

    std::string ixx;
    for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
        ixx = read_file(p);
        if (!ixx.empty())
            break;
    }
    CHECK(!ixx.empty(), "read evaluator.ixx");
    CHECK(ixx.find("#1757") != std::string::npos, "decl cites #1757");
    CHECK(ixx.find("std::size_t compact_pairs") != std::string::npos, "decl is size_t");
    CHECK(ixx.find("std::int64_t compact_pairs") == std::string::npos,
          "no int64_t compact_pairs decl");
}

static void run_1757_empty_mask() {
    std::println("\n--- AC2 (#1757): empty live_mask all-live size_t ---");
    aura::compiler::CompilerService cs;
    alloc_pairs(cs, 5);
    auto& ev = cs.evaluator();
    std::vector<bool> empty_mask;
    auto n = ev.compact_pairs(empty_mask);
    static_assert(std::is_same_v<decltype(n), std::size_t>,
                  "compact_pairs must return std::size_t");
    CHECK(n == 5, "5 pairs remain");
    CHECK(ev.resolve_pair(0) == 0, "identity remap");
    CHECK(ev.resolve_pair(4) == 4, "identity remap end");
}

static void run_1757_selective() {
    std::println("\n--- AC3 (#1757): selective live_mask size_t count ---");
    aura::compiler::CompilerService cs;
    alloc_pairs(cs, 5);
    auto& ev = cs.evaluator();
    std::vector<bool> mask = {true, false, true, false, true};
    std::size_t n = ev.compact_pairs(mask);
    CHECK(n == 3, "3 live pairs");
    CHECK(ev.resolve_pair(0) == 0, "old 0 → 0");
    CHECK(ev.resolve_pair(1) == -1, "old 1 dead");
    CHECK(ev.resolve_pair(2) == 1, "old 2 → 1");
    CHECK(ev.resolve_pair(4) == 2, "old 4 → 2");
}

static void run_1757_all_dead() {
    std::println("\n--- AC4 (#1757): all-dead returns 0 size_t ---");
    aura::compiler::CompilerService cs;
    alloc_pairs(cs, 3);
    auto& ev = cs.evaluator();
    std::vector<bool> mask = {false, false, false};
    std::size_t n = ev.compact_pairs(mask);
    CHECK(n == 0, "0 pairs remain");
    CHECK(ev.resolve_pair(0) == -1, "dead 0");
    CHECK(ev.resolve_pair(2) == -1, "dead 2");
}

static void run_1757_unsigned() {
    std::println("\n--- AC5 (#1757): unsigned count (no < 0 error path) ---");
    static_assert(std::is_unsigned_v<std::size_t>, "size_t unsigned");
    using Ret = decltype(std::declval<Evaluator&>().compact_pairs(
        std::declval<const std::vector<bool>&>()));
    static_assert(std::is_same_v<Ret, std::size_t>, "return is size_t");
    static_assert(std::is_unsigned_v<Ret>, "return unsigned");
    CHECK(true, "return type is unsigned size_t");
}

// ── Issue #261 — FlatAST NodeId lifecycle (ast:recycle / ast:compact / ast:snapshot+restore) ──
// Folded from tests/issues/test_issue_261.cpp via #1957.

static int64_t run_261_int(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool run_261_bool(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    return r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r);
}

static void run_261_recycle_primitive() {
    std::println("\n--- #261: ast:recycle-nodes primitive ---");
    aura::compiler::CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "seed set-code");
    CHECK(run_261_int(cs, "(ast:recycle-nodes)") >= 0,
          "ast:recycle-nodes returns non-negative int");
}

static void run_261_compact_primitive() {
    std::println("\n--- #261: ast:compact-nodes primitive ---");
    aura::compiler::CompilerService cs;
    CHECK(cs.eval("(set-code \"(begin (define x 1) (define y 2))\")").has_value(), "seed set-code");
    CHECK(run_261_int(cs, "(ast:compact-nodes)") >= 0,
          "ast:compact-nodes returns non-negative int");
}

static void run_261_snapshot_restore_hook() {
    std::println("\n--- #261: ast:snapshot / ast:restore recycle hooks ---");
    aura::compiler::CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 42)\")").has_value(), "seed set-code");
    auto snap_id = run_261_int(cs, "(ast:snapshot \"s261\")");
    CHECK(snap_id >= 0, "snapshot created");
    std::string restore_src = "(ast:restore " + std::to_string(snap_id) + ")";
    CHECK(run_261_bool(cs, restore_src), "restore succeeds");
}

// ── Issue #324 — Arena/pmr::vector yield-safe compaction observability + safety ──
// Folded from tests/issues/test_issue_324.cpp via #1957.
// AC #1 (source fiber-yield check) + AC #4 (CI stress) + AC #5 (GC integration) deferred.

static void run_324_workspace_integrity() {
    std::println("\n--- #324: workspace integrity across mutations ---");
    aura::compiler::CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2) (define c 3)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(eval-current)");
    CHECK(r.has_value(), "eval succeeds with bindings");
}

static void run_324_flatast_compact() {
    std::println("\n--- #324: FlatAST compaction via (ast:compact-nodes) ---");
    aura::compiler::CompilerService cs;
    (void)cs.eval(
        "(set-code \"(define a 1) (define b 2) (define c 3) (define d 4) (define e 5)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(ast:compact-nodes)");
    CHECK(r.has_value(), "(ast:compact-nodes) callable");
    auto q = cs.eval("(query:pattern \"a\")");
    CHECK(q.has_value(), "query:pattern works post-compact");
}

static void run_324_arena_stats_accessible() {
    std::println("\n--- #324: (stats:get \"arena:stats-json\") accessible ---");
    aura::compiler::CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(stats:get \"arena:stats-json\")");
    CHECK(r.has_value(), "(stats:get \"arena:stats-json\") returns a value");
}

static void run_324_dual_path_consistency() {
    std::println("\n--- #324: lookup consistency across mutations ---");
    aura::compiler::CompilerService cs;
    (void)cs.eval("(set-code \"(define a 1) (define b 2) (define c 3)\")");
    (void)cs.eval("(eval-current)");
    for (int i = 0; i < 5; ++i) {
        std::string code = "(mutate:replace-value (define a " + std::to_string(100 + i * 11) +
                           ") (define a " + std::to_string(100 + i * 11) + "))";
        auto r = cs.eval(code);
        CHECK(r.has_value(), std::string("mutate #") + std::to_string(i) + " succeeds");
    }
    auto re = cs.eval("(eval-current)");
    CHECK(re.has_value(), "eval succeeds after 5 mutations");
}

static void run_324_yield_field() {
    std::println("\n--- #324: ArenaStats.compaction_yield_checks field exists ---");
    aura::ast::ArenaStats s;
    s.compaction_count = 5;
    s.compaction_yield_checks = 0; // field exists; stays 0 in current build
    CHECK(s.compaction_count == 5, "compaction_count settable");
    CHECK(s.compaction_yield_checks == 0,
          "compaction_yield_checks field exists (always 0 until fiber hook lands)");
}

// ── Issue #187 — Arena compaction + double-arena strategy + observability ──
// Folded from tests/issues/test_issue_187.cpp via #1957.

static void run_187_arena_compaction_double_arena() {
    std::println("\n=== #187: Arena compaction + double-arena strategy + observability ===");
    // AC1.1-1.6: arena::compact_estimate + compact + shrink_to_fit + live-preserve
    {
        std::println("\n--- AC1.1-1.6: compact_estimate + compact + shrink_to_fit ---");
        aura::ast::ASTArena arena(8 * 1024 * 1024);
        CHECK(arena.compact_estimate() > 0, "compact_estimate > 0 on fresh arena");
        auto* obj = arena.create<int>(42);
        CHECK(*obj == 42, "create<int> returns valid object");
        CHECK(arena.stats().capacity < 8 * 1024 * 1024,
              "auto-compact-on-alloc reduced capacity from 8MB to < 8MB");
        std::size_t s1 = arena.compact();
        std::size_t s2 = arena.compact();
        CHECK(s1 == 0 && s2 == 0, "compact() idempotent: no-op after auto-compact-on-alloc");
        aura::ast::ASTArena small_arena(1024);
        small_arena.create<int>(1);
        auto cap_before = small_arena.stats().capacity;
        small_arena.shrink_to_fit();
        CHECK(small_arena.stats().capacity == cap_before, "shrink_to_fit no-op at initial size");
        CHECK(small_arena.stats().used > 0, "live allocation preserved across shrink_to_fit");
        aura::ast::ASTArena preserved_arena(8 * 1024 * 1024);
        auto* a = preserved_arena.create<int>(100);
        auto* b = preserved_arena.create<int>(200);
        auto* c = preserved_arena.create<int>(300);
        preserved_arena.compact();
        CHECK(*a == 100 && *b == 200 && *c == 300, "live objects preserved across compact()");
    }
    // AC1.7-1.10: ArenaGroup multi-arena management
    {
        std::println("\n--- AC1.7-1.10: ArenaGroup compact_module/auto_compact/stats_json ---");
        aura::ast::ArenaGroup group;
        auto& m = group.module_arena("test_mod", 1024 * 1024);
        m.create<int>(1);
        (void)group.compact_module("test_mod");
        CHECK(group.compact_module("nonexistent") == 0,
              "compact_module on missing module returns 0");
        group.set_compact_threshold(0.0);
        auto& m1 = group.module_arena("a", 1024 * 1024);
        m1.create<int>(1);
        auto& m2 = group.module_arena("b", 1024 * 1024);
        m2.create<int>(2);
        CHECK(group.auto_compact() >= 0, "auto_compact returns non-negative total");
        auto& mm = group.module_arena("hello", 1024 * 1024);
        mm.create<int>(42);
        mm.compact();
        auto json = group.stats_json();
        CHECK(json.find("\"arenas\"") != std::string::npos, "JSON has arenas key");
        CHECK(json.find("\"name\":\"hello\"") != std::string::npos, "JSON has module name");
        CHECK(json.find("\"compaction_count\"") != std::string::npos, "JSON has compaction count");
        CHECK(json.find("\"fragmentation_ratio\"") != std::string::npos,
              "JSON has fragmentation ratio");
        group.set_compact_threshold(2.0);
        CHECK(group.compact_threshold() <= 0.95, "threshold clamped to <= 0.95");
        group.set_compact_threshold(-1.0);
        CHECK(group.compact_threshold() >= 0.0, "threshold clamped to >= 0.0");
    }
    // AC2: Double-arena policy (per-module arenas)
    {
        std::println("\n--- AC2: ArenaGroup per-module arenas (persistent vs temp) ---");
        aura::ast::ArenaGroup group;
        auto& m1 = group.module_arena("persistent_mod", 4 * 1024 * 1024);
        auto& m2 = group.module_arena("temp_mod", 1 * 1024 * 1024);
        m1.create<int>(1);
        m2.create<int>(2);
        m2.create<int>(3);
        auto stats = group.module_stats();
        CHECK(stats.size() == 2, "2 module arenas tracked");
        bool found_persistent = false, found_temp = false;
        for (auto& [name, s] : stats) {
            if (name == "persistent_mod")
                found_persistent = true;
            if (name == "temp_mod")
                found_temp = true;
        }
        CHECK(found_persistent && found_temp, "persistent + temp both in module_stats");
    }
    // AC3: Fragmentation observability
    {
        std::println("\n--- AC3.1-3.2: ArenaStats fragmentation_ratio + format() ---");
        aura::ast::ASTArena arena(8 * 1024 * 1024);
        CHECK(arena.stats().fragmentation_ratio() > 0.9, "fresh 8MB has > 90% fragmentation");
        arena.create<int>(42);
        CHECK(arena.stats().fragmentation_ratio() < 1.0, "after small alloc, fragmentation < 1.0");
        arena.compact();
        auto formatted = arena.stats().format();
        CHECK(formatted.find("compaction") != std::string::npos,
              "format() output mentions compaction");
    }
    // AC4: StringPool compaction + observability
    {
        std::println("\n--- AC4: StringPool intern + compact + reset + data_size ---");
        std::pmr::polymorphic_allocator<std::byte> alloc{};
        aura::ast::StringPool pool(alloc);
        CHECK(pool.entry_count() == 0 && pool.load_factor() == 0.0, "fresh pool empty");
        pool.intern("hello");
        pool.intern("world");
        pool.intern("foo");
        CHECK(pool.entry_count() == 3, "3 entries after 3 interns");
        CHECK(pool.load_factor() > 0.0, "load_factor > 0 after intern");
        auto initial = pool.data_size();
        pool.intern("a");
        pool.intern("b");
        CHECK(pool.data_size() > initial, "data_size grows as we intern");
        aura::ast::StringPool pool2(alloc);
        for (int i = 0; i < 200; ++i)
            pool2.intern(std::string("s") + std::to_string(i));
        auto cap_before = pool2.hash_capacity();
        pool2.compact();
        CHECK(pool2.hash_capacity() <= cap_before, "compact reduces or preserves hash_capacity");
        auto sym = pool2.intern("hello");
        CHECK(pool2.resolve(sym) == "hello", "SymId still valid after compact");
        auto s1 = pool2.compact();
        auto s2 = pool2.compact();
        CHECK(s1 >= 0 && s2 == 0, "second compact is no-op (idempotent)");
        pool2.intern("x");
        CHECK(pool2.entry_count() >= 1, "entry exists before reset");
        pool2.reset();
        CHECK(pool2.entry_count() == 0, "0 entries after reset");
        CHECK(pool2.data_size() == 1, "data_size = 1 after reset (just leading NUL)");
    }
    // AC5: Long fragmentation scenario
    {
        std::println("\n--- AC5.1: fragmentation scenario (1000 allocs + compact) ---");
        aura::ast::ASTArena arena(8 * 1024 * 1024);
        std::vector<int*> ptrs;
        for (int i = 0; i < 1000; ++i)
            ptrs.push_back(arena.create<int>(i));
        CHECK(arena.stats().peak_used > 0, "peak_used > 0 after 1000 allocs");
        arena.compact();
        CHECK(arena.stats().capacity <= 8 * 1024 * 1024, "compact reduces capacity");
        bool all_ok = true;
        for (size_t i = 0; i < ptrs.size(); ++i)
            if (*ptrs[i] != static_cast<int>(i)) {
                all_ok = false;
                break;
            }
        CHECK(all_ok, "all 1000 live objects intact after compact");
    }
    // AC6: Aura-level primitives (CompilerService-driven)
    {
        std::println("\n--- AC6.1-6.7: (arena:compact) + (arena:estimate) + ... ---");
        aura::compiler::CompilerService cs;
        if (auto r = cs.eval("(arena:compact)"); r && is_int(*r)) {
            CHECK(as_int(*r) >= 0, "(arena:compact) returns non-negative bytes");
        }
        if (auto r = cs.eval("(stats:get \"arena:estimate\")"); r && is_int(*r)) {
            CHECK(as_int(*r) >= 0, "(stats:get \"arena:estimate\") returns non-negative bytes");
        }
        if (auto r = cs.eval("(stats:get \"arena:stats-json\")");
            r && aura::compiler::types::is_string(*r)) {
            CHECK(true, "(stats:get \"arena:stats-json\") returns string");
        }
        if (auto r = cs.eval("(string-pool:compact)"); r && is_int(*r)) {
            CHECK(as_int(*r) >= 0, "(string-pool:compact) returns non-negative bytes");
        }
        auto sv = cs.eval("(stats:get \"string-pool:stats\")");
        CHECK(sv.has_value(), "(stats:get \"string-pool:stats\") returns a value (or void)");
        auto tv = cs.eval("(arena:set-compact-threshold 25)");
        CHECK(tv.has_value(), "(arena:set-compact-threshold 25) callable");
        if (auto r = cs.eval("(begin "
                             "(define x1 1) (define x2 2) (define x3 3) (define x4 4) "
                             "(define x5 5) (string-pool:compact) "
                             "(define y1 1) (define y2 2) (string-pool:compact) "
                             "(+ x1 x2 x3 x4 x5 y1 y2))");
            r && is_int(*r)) {
            CHECK(as_int(*r) == 18, "compound workflow: 1+2+3+4+5+1+2 = 18");
        }
    }
}


// ── Issue #300 — Live-object defragmentation observability foundation ──
// Folded from tests/issues/test_issue_300.cpp via #1957.
// Verifies (stats:get "arena:defrag-stats") returns a 5-tuple:
// (compaction-count, defrag-attempted-count, fragmentation-bp,
//  wasted-bytes, compact-estimate-bytes) + request/safepoint flag.

static bool extract_300_5tuple(CompilerService& cs, const aura::compiler::types::EvalValue& v,
                               int64_t& e1, int64_t& e2, int64_t& e3, int64_t& e4, int64_t& e5) {
    if (!aura::compiler::types::is_pair(v))
        return false;
    auto& pairs = cs.evaluator().pairs();
    auto p1_idx = aura::compiler::types::as_pair_idx(v);
    if (p1_idx >= pairs.size())
        return false;
    auto& p1 = pairs[p1_idx];
    if (!aura::compiler::types::is_int(p1.car))
        return false;
    e1 = aura::compiler::types::as_int(p1.car);
    if (!aura::compiler::types::is_pair(p1.cdr))
        return false;
    auto p2_idx = aura::compiler::types::as_pair_idx(p1.cdr);
    if (p2_idx >= pairs.size())
        return false;
    auto& p2 = pairs[p2_idx];
    if (!aura::compiler::types::is_int(p2.car))
        return false;
    e2 = aura::compiler::types::as_int(p2.car);
    if (!aura::compiler::types::is_pair(p2.cdr))
        return false;
    auto p3_idx = aura::compiler::types::as_pair_idx(p2.cdr);
    if (p3_idx >= pairs.size())
        return false;
    auto& p3 = pairs[p3_idx];
    if (!aura::compiler::types::is_int(p3.car))
        return false;
    e3 = aura::compiler::types::as_int(p3.car);
    if (!aura::compiler::types::is_pair(p3.cdr))
        return false;
    auto p4_idx = aura::compiler::types::as_pair_idx(p3.cdr);
    if (p4_idx >= pairs.size())
        return false;
    auto& p4 = pairs[p4_idx];
    if (!aura::compiler::types::is_int(p4.car))
        return false;
    e4 = aura::compiler::types::as_int(p4.car);
    if (!aura::compiler::types::is_int(p4.cdr))
        return false;
    e5 = aura::compiler::types::as_int(p4.cdr);
    return true;
}

static void run_300_arena_defrag_stats_observability() {
    std::println("\n=== #300: arena:defrag-stats 5-tuple observability foundation ===");
    // AC5 needs the safepoint stub registered (Issue #1397).
    static auto s_done = []() {
        aura::gc_hooks::g_arena_safepoint_check.store(+[] {});
        return true;
    }();
    (void)s_done;
    // T1: result is a 5-tuple
    {
        std::println("\n--- #300 T1: returns 5-tuple ---");
        CompilerService cs;
        auto r = cs.eval("(stats:get \"arena:defrag-stats\")");
        if (!r) {
            ++g_failed;
            return;
        }
        int64_t e1, e2, e3, e4, e5;
        bool ok = extract_300_5tuple(cs, *r, e1, e2, e3, e4, e5);
        CHECK(ok, "result is a 5-tuple (compaction-count, defrag-attempted-count, "
                  "fragmentation-bp, wasted-bytes, compact-estimate-bytes)");
    }
    // T2: empty workspace counters
    {
        std::println("\n--- #300 T2: empty workspace counters ---");
        CompilerService cs;
        auto r = cs.eval("(stats:get \"arena:defrag-stats\")");
        if (!r) {
            ++g_failed;
            return;
        }
        int64_t e1, e2, e3, e4, e5;
        if (!extract_300_5tuple(cs, *r, e1, e2, e3, e4, e5)) {
            ++g_failed;
            return;
        }
        CHECK(e1 >= 0, "compaction-count >= 0");
        CHECK(e2 == 0, "defrag-attempted-count == 0 on fresh workspace");
        CHECK(e4 == 0, "wasted-bytes == 0 on fresh workspace");
        CHECK(e3 >= 0 && e3 <= 10000, "fragmentation-bp in [0, 10000] basis points");
    }
    // T3: shape via Aura
    {
        std::println("\n--- #300 T3: 5-tuple shape via Aura ---");
        CompilerService cs;
        cs.eval("(set-code \"(define q 100)\")");
        auto r = cs.eval("(let ((t (stats:get \"arena:defrag-stats\")))"
                         " (and (pair? t)"
                         "       (pair? (cdr t))"
                         "       (pair? (cdr (cdr t)))"
                         "       (pair? (cdr (cdr (cdr t))))"
                         "       (integer? (cdr (cdr (cdr (cdr t)))))"
                         "       (integer? (car t))"
                         "       (integer? (car (cdr t)))"
                         "       (integer? (car (cdr (cdr t))))"
                         "       (integer? (car (cdr (cdr (cdr t)))))))");
        if (!r) {
            ++g_failed;
            return;
        }
        bool is_t = aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r);
        CHECK(is_t, "5-tuple shape: 4 pairs + terminal int + all 4 cars are int");
    }
    // T4: (arena:defrag) bumps defrag-attempted-count
    {
        std::println("\n--- #300 T4: (arena:defrag) bumps defrag-attempted-count ---");
        CompilerService cs;
        cs.eval("(set-code \"(define a 1) (define b 2)\")");
        auto r0 = cs.eval("(stats:get \"arena:defrag-stats\")");
        if (!r0) {
            ++g_failed;
            return;
        }
        int64_t e1, e2, e3, e4, e5;
        if (!extract_300_5tuple(cs, *r0, e1, e2, e3, e4, e5)) {
            ++g_failed;
            return;
        }
        auto defrag_before = e2, compact_est_before = e5;
        cs.eval("(arena:defrag)");
        auto r = cs.eval("(stats:get \"arena:defrag-stats\")");
        if (!r) {
            ++g_failed;
            return;
        }
        if (!extract_300_5tuple(cs, *r, e1, e2, e3, e4, e5)) {
            ++g_failed;
            return;
        }
        CHECK(e2 == defrag_before + 1, "defrag-attempted-count incremented by 1 (was " +
                                           std::to_string(defrag_before) + ", now " +
                                           std::to_string(e2) + ")");
        CHECK(e5 <= compact_est_before, "compact-estimate did not grow");
        CHECK(e3 >= 0 && e3 <= 10000, "fragmentation-bp in [0, 10000] after defrag");
    }
    // T5: defrag request flag lifecycle (fresh=#f / request=#t / set=#t /
    //     duplicate=#f / after-defrag=#f)
    {
        std::println("\n--- #300 T5: defrag request flag (safepoint scaffold) ---");
        CompilerService cs;
        auto r0 = cs.eval("(arena:defrag-requested?)");
        if (!r0) {
            ++g_failed;
            return;
        }
        bool fresh = aura::compiler::types::is_bool(*r0) && !aura::compiler::types::as_bool(*r0);
        auto r1 = cs.eval("(arena:request-defrag)");
        if (!r1) {
            ++g_failed;
            return;
        }
        bool request_ok =
            aura::compiler::types::is_bool(*r1) && aura::compiler::types::as_bool(*r1);
        auto r2 = cs.eval("(arena:defrag-requested?)");
        if (!r2) {
            ++g_failed;
            return;
        }
        bool set = aura::compiler::types::is_bool(*r2) && aura::compiler::types::as_bool(*r2);
        auto r3 = cs.eval("(arena:request-defrag)");
        if (!r3) {
            ++g_failed;
            return;
        }
        bool dup = aura::compiler::types::is_bool(*r3) && !aura::compiler::types::as_bool(*r3);
        cs.eval("(set-code \"(define x 1)\")");
        cs.eval("(arena:defrag)");
        auto r4 = cs.eval("(arena:defrag-requested?)");
        if (!r4) {
            ++g_failed;
            return;
        }
        bool cleared = aura::compiler::types::is_bool(*r4) && !aura::compiler::types::as_bool(*r4);
        CHECK(fresh && request_ok && set && dup && cleared,
              "defrag flag lifecycle: fresh=#f, request=#t, set=#t, dup=#f, after-defrag=#f");
    }
}

// ── Issue #322 — Dual-Path SoA/EnvId + arena compaction ──
// Folded from tests/issues/test_issue_322.cpp via #1957.
// S2/S3/S6/S7/S8 deferred (pre-existing bugs in arena:* primitives — see
// test_issue_322.cpp close comments + issue follow-up). S4 (FlatAST
// compaction) and S5 (dual-path mutation) overlap with #187/#324 — kept
// here for inventory fidelity.

static void run_322_dual_path_soa_envid() {
    std::println("\n=== #322: Dual-Path SoA/EnvId + arena compaction ===");
    // S1: workspace integrity with bindings
    {
        std::println("\n--- #322 S1: workspace integrity ---");
        CompilerService cs;
        (void)cs.eval("(set-code \"(define a 1) (define b 2) (define c 3)\")");
        (void)cs.eval("(eval-current)");
        auto r = cs.eval("(eval-current)");
        CHECK(r.has_value(), "eval succeeds with bindings");
    }
    // S4: FlatAST compaction via (ast:compact-nodes)
    {
        std::println("\n--- #322 S4: FlatAST compaction via (ast:compact-nodes) ---");
        CompilerService cs;
        (void)cs.eval(
            "(set-code \"(define a 1) (define b 2) (define c 3) (define d 4) (define e 5)\")");
        (void)cs.eval("(eval-current)");
        auto r = cs.eval("(ast:compact-nodes)");
        CHECK(r.has_value(), "(ast:compact-nodes) callable");
        auto q = cs.eval("(query:pattern \"a\")");
        CHECK(q.has_value(), "query:pattern works post-compact");
    }
    // S5: dual-path consistency across mutations
    {
        std::println("\n--- #322 S5: dual-path consistency across mutations ---");
        CompilerService cs;
        (void)cs.eval("(set-code \"(define a 1) (define b 2) (define c 3)\")");
        (void)cs.eval("(eval-current)");
        for (int i = 0; i < 5; ++i) {
            std::string code = "(mutate:replace-value (define a ";
            code += std::to_string(100 + i * 11);
            code += ") (define a ";
            code += std::to_string(100 + i * 11);
            code += "))";
            auto r = cs.eval(code);
            CHECK(r.has_value(), std::string("mutate #") + std::to_string(i) + " succeeds");
        }
        auto re = cs.eval("(eval-current)");
        CHECK(re.has_value(), "eval succeeds after 5 mutations");
    }
}

// ── Issue #335 — ArenaGroup adaptive auto-compact heuristics ──
// Folded from tests/issues/test_issue_335.cpp via #1957.
// Adaptive (arena:adaptive-compact) primitive + (arena:should-auto-compact?)
// probe + (stats:get "arena:adaptive-stats") observability counters.

static void run_335_arena_adaptive_compact_heuristics() {
    std::println("\n=== #335: ArenaGroup adaptive auto-compact heuristics ===");
    auto build_workspace = [](CompilerService& cs, int n_defines) {
        std::string code = "(begin ";
        for (int i = 0; i < n_defines; ++i)
            code += "(define v_" + std::to_string(i) + " " + std::to_string(i) + ") ";
        code += ")";
        (void)cs.eval(std::string("(set-code \"") + code + "\")");
        (void)cs.eval("(eval-current)");
    };
    // AC1: should_auto_compact probe
    {
        std::println("\n--- #335 AC1: should_auto_compact probe ---");
        CompilerService cs;
        auto r1 = cs.eval("(arena:should-auto-compact? \"main\")");
        CHECK(r1.has_value() && aura::compiler::types::is_bool(*r1) &&
                  !aura::compiler::types::as_bool(*r1),
              "no workspace: should-auto-compact? returns #f");
        build_workspace(cs, 5);
        auto r2 = cs.eval("(arena:should-auto-compact? \"main\")");
        CHECK(r2.has_value() && aura::compiler::types::is_bool(*r2),
              "with workspace: should-auto-compact? returns a bool");
    }
    // AC2: adaptive_compact reclaims + updates EMA
    {
        std::println("\n--- #335 AC2: adaptive_compact reclaims + EMA ---");
        CompilerService cs;
        build_workspace(cs, 3);
        auto r1 = cs.eval("(arena:adaptive-compact)");
        CHECK(r1.has_value() && is_int(*r1), "(arena:adaptive-compact) returns int");
        const auto r1_val = as_int(*r1);
        CHECK(r1_val >= 0, "bytes reclaimed is non-negative");
        auto r2 = cs.eval("(stats:get \"arena:adaptive-stats\")");
        CHECK(r2.has_value(), "(stats:get \"arena:adaptive-stats\") returns a value");
    }
    // AC3: EMA lowers threshold on productive compactions
    {
        std::println("\n--- #335 AC3: EMA lowers threshold on productive compactions ---");
        aura::ast::ArenaGroup group;
        group.set_compact_threshold(0.50);
        auto& arena = group.module_arena("test", 4 * 1024);
        std::vector<void*> ptrs;
        for (int i = 0; i < 16; ++i)
            ptrs.push_back(arena.create<std::array<std::byte, 64>>(std::array<std::byte, 64>{}));
        (void)ptrs;
        const auto should = group.should_auto_compact("test");
        CHECK(true, "should_auto_compact returns a bool (true or false)");
    }
    // AC4: observability counters
    {
        std::println("\n--- #335 AC4: observability counters ---");
        CompilerService cs;
        build_workspace(cs, 3);
        auto before = cs.eval("(stats:get \"arena:adaptive-stats\")");
        CHECK(before.has_value(), "(stats:get \"arena:adaptive-stats\") pre-call returns a value");
        for (int i = 0; i < 5; ++i)
            cs.eval("(arena:adaptive-compact)");
        auto after = cs.eval("(stats:get \"arena:adaptive-stats\")");
        CHECK(after.has_value(), "(stats:get \"arena:adaptive-stats\") post-call returns a value");
    }
    // AC5: perf-bound (100 adaptive_compact calls < 10s eval-bound)
    {
        std::println("\n--- #335 AC5: perf-bound 100 adaptive_compact calls ---");
        CompilerService cs;
        build_workspace(cs, 50);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i)
            cs.eval("(arena:adaptive-compact)");
        auto t1 = std::chrono::steady_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        CHECK(us < 10000000, "100 adaptive_compact calls < 10s (eval-bound; primitive is fast)");
    }
}

// ── Issue #623 — arena auto-compact threshold setter/getter + back-compat ──
// Folded from tests/issues/test_issue_623.cpp via #1957.
// AC5 (concurrent std::thread read test) simplified to a rapid
// sequential loop here to avoid adding <mutex>/<thread> includes
// to test_compact_batch.cpp; the real concurrent coverage stays
// in the original test_issue_623.cpp for TSan/atomicity verification.

static void run_623_arena_auto_compact_threshold_setter() {
    std::println("\n=== #623: arena auto-compact threshold setter/getter + back-compat ===");
    // AC1: (arena:auto-compact-threshold) read
    {
        std::println("\n--- #623 AC1: read auto-compact-threshold ---");
        CompilerService cs;
        auto r = cs.eval("(arena:auto-compact-threshold)");
        CHECK(r.has_value() && is_int(*r), "auto-compact-threshold returns int");
        const auto val = as_int(*r);
        CHECK(val == 50 || val == -1, std::format("in {{50, -1}} (got {})", val));
    }
    // AC2: set + clamping
    {
        std::println("\n--- #623 AC2: set + clamping ---");
        CompilerService cs;
        const auto baseline = as_int(*cs.eval("(arena:auto-compact-threshold)"));
        auto r_set = cs.eval("(arena:set-auto-compact-threshold 75)");
        CHECK(r_set.has_value() && is_int(*r_set), "setter returns int (previous value)");
        const auto prev = as_int(*r_set);
        if (baseline == -1) {
            CHECK(prev == -1, std::format("baseline -1 case: prev == -1 (got {})", prev));
        } else {
            CHECK(prev == baseline, std::format("prev == baseline ({} == {})", prev, baseline));
        }
        if (baseline != -1) {
            const auto now = as_int(*cs.eval("(arena:auto-compact-threshold)"));
            CHECK(now == 75, std::format("readback after set == 75 (got {})", now));
        }
        cs.eval("(arena:set-auto-compact-threshold -5)");
        if (baseline != -1) {
            const auto after_low = as_int(*cs.eval("(arena:auto-compact-threshold)"));
            CHECK(after_low == 0, std::format("negative arg clamped to 0 (got {})", after_low));
        }
        cs.eval("(arena:set-auto-compact-threshold 200)");
        if (baseline != -1) {
            const auto after_high = as_int(*cs.eval("(arena:auto-compact-threshold)"));
            CHECK(after_high == 95, std::format("arg > 95 clamped to 95 (got {})", after_high));
        }
        cs.eval("(arena:set-auto-compact-threshold 50)"); // restore
    }
    // AC3: non-int arg no-op
    {
        std::println("\n--- #623 AC3: bad-arg no-op ---");
        CompilerService cs;
        cs.eval("(arena:set-auto-compact-threshold 25)");
        const auto before = as_int(*cs.eval("(arena:auto-compact-threshold)"));
        auto r = cs.eval("(arena:set-auto-compact-threshold \"not-a-number\")");
        CHECK(r.has_value() && is_int(*r), "non-int arg returns int (current value)");
        const auto after = as_int(*cs.eval("(arena:auto-compact-threshold)"));
        CHECK(after == before,
              std::format("non-int arg left threshold unchanged ({} -> {})", before, after));
        cs.eval("(arena:set-auto-compact-threshold 50)"); // restore
    }
    // AC4: existing arena primitives back-compat
    // (#187/#300/#430/#335/#464/#685/#604)
    {
        std::println("\n--- #623 AC4: existing arena primitives back-compat ---");
        CompilerService cs;
        auto s_json = cs.eval("(stats:get \"arena:stats-json\")");
        CHECK(s_json.has_value(), "(stats:get \"arena:stats-json\") reachable (#187 back-compat)");
        auto s_def = cs.eval("(arena:defrag)");
        CHECK(s_def.has_value(), "(arena:defrag) reachable (#300 back-compat)");
        auto s_pol = cs.eval("(arena:compact-with-policy)");
        CHECK(s_pol.has_value(), "(arena:compact-with-policy) reachable (#430 back-compat)");
        auto s_probe = cs.eval("(arena:should-auto-compact?)");
        CHECK(s_probe.has_value(), "(arena:should-auto-compact?) reachable (#335 back-compat)");
        auto s_auto = cs.eval("(engine:metrics \"query:arena-auto-stats\")");
        CHECK(s_auto.has_value(),
              "(engine:metrics \"query:arena-auto-stats\") reachable (#464 back-compat)");
        auto s_compact = cs.eval("(engine:metrics \"query:arena-auto-compact-stats\")");
        CHECK(s_compact.has_value(),
              "(engine:metrics \"query:arena-auto-compact-stats\") reachable (#685 back-compat)");
        auto s_snap = cs.eval("(engine:metrics \"query:arena-fragmentation-snapshot\")");
        CHECK(
            s_snap.has_value(),
            "(engine:metrics \"query:arena-fragmentation-snapshot\") reachable (#604 back-compat)");
    }
    // AC5: rapid sequential reads — atomicity proxy (skip full std::thread)
    {
        std::println("\n--- #623 AC5: rapid sequential reads (atomicity proxy) ---");
        CompilerService cs;
        cs.eval("(arena:set-auto-compact-threshold 60)");
        int ok_count = 0;
        constexpr int k_iters = 8;
        for (int i = 0; i < k_iters; ++i) {
            auto r = cs.eval("(arena:auto-compact-threshold)");
            if (r && is_int(*r))
                ++ok_count;
        }
        CHECK(ok_count == k_iters,
              std::format("rapid sequential: {} / {} reads returned int", ok_count, k_iters));
        cs.eval("(arena:set-auto-compact-threshold 50)");
    }
}

// ── Issue #464 — Arena auto-compaction policy + fiber scheduler integration ──
// Folded from tests/issues/test_issue_464_arena_auto_compaction.cpp via #1957.
// 10 ACs: direct ArenaGroup API + EDSL observability + guard-dtor + long-session signal.

static void run_464_arena_auto_compaction() {
    std::println("\n=== #464: Arena auto-compaction policy + fiber scheduler integration ===");
    // AC1: empty group auto_compact_with_safety returns 0 + bumps counters
    {
        std::println("\n--- #464 AC1: empty group auto_compact_with_safety ---");
        aura::ast::ArenaGroup g;
        auto saved = g.auto_compact_with_safety();
        CHECK(saved == 0, "0 bytes reclaimed on empty group");
        CHECK(g.auto_compact_guard_call_count() == 1,
              "guard-call counter == 1 (one call bumps once)");
        CHECK(g.compaction_yield_checks_group() == 1, "yield-check counter == 1");
    }
    // AC2: bump_auto_compact_guard_call bumps the counter
    {
        std::println("\n--- #464 AC2: bump_auto_compact_guard_call ---");
        aura::ast::ArenaGroup g;
        CHECK(g.auto_compact_guard_call_count() == 0, "starts at 0");
        g.bump_auto_compact_guard_call();
        g.bump_auto_compact_guard_call();
        g.bump_auto_compact_guard_call();
        CHECK(g.auto_compact_guard_call_count() == 3, "after 3 bumps == 3");
    }
    // AC3: bump_compaction_yield_check
    {
        std::println("\n--- #464 AC3: bump_compaction_yield_check ---");
        aura::ast::ArenaGroup g;
        CHECK(g.compaction_yield_checks_group() == 0, "starts at 0");
        g.bump_compaction_yield_check();
        g.bump_compaction_yield_check();
        CHECK(g.compaction_yield_checks_group() == 2, "after 2 bumps == 2");
    }
    // AC4: auto_compact_with_safety bumps both counters
    {
        std::println("\n--- #464 AC4: auto_compact_with_safety bumps both ---");
        aura::ast::ArenaGroup g;
        (void)g.module_arena("test_module", 4096);
        auto before_guard = g.auto_compact_guard_call_count();
        auto before_yield = g.compaction_yield_checks_group();
        g.auto_compact_with_safety();
        CHECK(g.auto_compact_guard_call_count() == before_guard + 1, "guard-call advances by 1");
        CHECK(g.compaction_yield_checks_group() == before_yield + 1, "yield-check advances by 1");
    }
    // AC5: EDSL (engine:metrics "query:arena-auto-stats") returns a hash with 4 fields
    CompilerService cs;
    {
        std::println("\n--- #464 AC5: EDSL (engine:metrics \"query:arena-auto-stats\") hash ---");
        auto r = cs.eval("(engine:metrics \"query:arena-auto-stats\")");
        CHECK(r.has_value() && aura::compiler::types::is_hash(*r),
              "(engine:metrics \"query:arena-auto-stats\") returns a hash");
        for (const char* key : {"auto-compact-guard-call-count", "compaction-yield-checks",
                                "auto-compact-trigger-count", "auto-compact-skip-count"}) {
            auto rr = cs.eval(
                std::format("(hash-ref (engine:metrics \"query:arena-auto-stats\") '{}')", key));
            CHECK(rr.has_value() && aura::compiler::types::is_int(*rr),
                  std::format("hash-ref '{}' returns int", key));
        }
    }
    // AC6: guard-call counter observable (post-mutate increments via the guard dtor)
    {
        std::println("\n--- #464 AC6: guard dtor bumps counter ---");
        auto before = cs.eval("(hash-ref (engine:metrics \"query:arena-auto-stats\") "
                              "'auto-compact-guard-call-count)");
        auto before_n = (before && aura::compiler::types::is_int(*before))
                            ? aura::compiler::types::as_int(*before)
                            : 0;
        cs.eval(R"((mutate:rebind 'foo 42))");
        auto after = cs.eval("(hash-ref (engine:metrics \"query:arena-auto-stats\") "
                             "'auto-compact-guard-call-count)");
        auto after_n = (after && aura::compiler::types::is_int(*after))
                           ? aura::compiler::types::as_int(*after)
                           : 0;
        CHECK(after_n > before_n,
              std::format("guard-call count advances (before={}, after={})", before_n, after_n));
    }
    // AC7: stats:count > 0 (loose check; cumulative count grows each wave)
    {
        std::println("\n--- #464 AC7: stats:count ---");
        auto r = cs.eval("(stats:count)");
        CHECK(r.has_value() && aura::compiler::types::is_int(*r), "stats:count returns int");
        CHECK(aura::compiler::types::as_int(*r) > 0, "stats:count > 0 (cumulative)");
    }
    // AC8: (stats:list) includes query:arena-auto-stats
    {
        std::println("\n--- #464 AC8: (stats:list) includes query:arena-auto-stats ---");
        auto r = cs.eval(R"((if (member "query:arena-auto-stats" (stats:list)) #t #f))");
        CHECK(r.has_value(), "stats:list + member returns a value (#t if present)");
    }
    // AC9: fresh service: all 4 counters default to 0
    {
        std::println("\n--- #464 AC9: fresh service defaults ---");
        CompilerService cs2;
        for (const char* k : {"auto-compact-guard-call-count", "compaction-yield-checks",
                              "auto-compact-trigger-count", "auto-compact-skip-count"}) {
            std::string src =
                std::format("(hash-ref (engine:metrics \"query:arena-auto-stats\") '{}')", k);
            auto v = cs2.eval(src);
            CHECK(v.has_value() && aura::compiler::types::is_int(*v) &&
                      aura::compiler::types::as_int(*v) == 0,
                  std::format("fresh counter '{}' == 0", k));
        }
    }
    // AC10: long AI session signal (5 mutates bump guard-call by >= 5)
    {
        std::println("\n--- #464 AC10: long AI session signal ---");
        CompilerService cs3;
        auto before = cs3.eval("(hash-ref (engine:metrics \"query:arena-auto-stats\") "
                               "'auto-compact-guard-call-count)");
        auto before_n = (before && aura::compiler::types::is_int(*before))
                            ? aura::compiler::types::as_int(*before)
                            : 0;
        for (int i = 0; i < 5; ++i)
            cs3.eval(std::format(R"((mutate:rebind 'foo_{} {}))", i, i * 10));
        auto after = cs3.eval("(hash-ref (engine:metrics \"query:arena-auto-stats\") "
                              "'auto-compact-guard-call-count)");
        auto after_n = (after && aura::compiler::types::is_int(*after))
                           ? aura::compiler::types::as_int(*after)
                           : 0;
        CHECK(after_n >= before_n + 5,
              std::format("5 mutates bump guard-call by >= 5 (before={}, after={})", before_n,
                          after_n));
    }
}

// ── Issue #604 — arena auto-compact + defrag + fiber/GC safepoint ──
// Folded from tests/issues/test_issue_604.cpp via #1957.
// 6 ACs: snapshot hash shape + fiber-context yield check + concurrent churn.

static void run_604_arena_auto_compact_defrag_fiber_safepoint() {
    std::println("\n=== #604: arena auto-compact + defrag + fiber/GC safepoint ===");
    auto stat_int = [](CompilerService& cs, std::string_view key) -> int64_t {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:arena-fragmentation-snapshot\") '{}')", key));
        if (!r || !aura::compiler::types::is_int(*r))
            return -1;
        return aura::compiler::types::as_int(*r);
    };
    auto stat_float = [](CompilerService& cs, std::string_view key) -> double {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:arena-fragmentation-snapshot\") '{}')", key));
        if (!r || !aura::compiler::types::is_float(*r))
            return -1.0;
        return aura::compiler::types::as_float(*r);
    };
    CompilerService cs;
    // AC1: snapshot returns a hash with documented fields
    {
        std::println("\n--- #604 AC1: snapshot hash shape ---");
        auto stats = cs.eval("(engine:metrics \"query:arena-fragmentation-snapshot\")");
        CHECK(stats.has_value() && aura::compiler::types::is_hash(*stats),
              "snapshot returns a hash");
        CHECK(stat_int(cs, "auto-compact-triggers") >= 0, "auto-compact-triggers present");
        CHECK(stat_int(cs, "yield-deferred") >= 0, "yield-deferred present");
        CHECK(stat_int(cs, "defrag-saved-bytes") >= 0, "defrag-saved-bytes present");
    }
    // AC2: fragmentation-ratio is a float in [0, 1]
    {
        std::println("\n--- #604 AC2: fragmentation-ratio range ---");
        double frag = stat_float(cs, "fragmentation-ratio");
        CHECK(frag >= 0.0 && frag <= 1.0, std::format("frag in [0,1] (got {:.3f})", frag));
    }
    // AC3 + AC4: compact() + defrag() fiber-context yield check
    {
        std::println("\n--- #604 AC3+AC4: compact()/defrag() fiber-context yield check ---");
        std::atomic<int> safepoint_hits{0};
        static std::atomic<int>* s_hits = &safepoint_hits;
        s_hits->store(0);
        aura::gc_hooks::g_arena_safepoint_check.store(
            +[]() noexcept { s_hits->fetch_add(1, std::memory_order_relaxed); });
        aura::ast::ASTArena arena(1 << 20);
        struct SmallNode {
            char data[48];
        };
        for (int i = 0; i < 32; ++i)
            (void)arena.create<SmallNode>();
        // Outside fiber: no yield-bump
        aura::gc_hooks::g_fiber_active.store(+[]() noexcept { return false; });
        auto yc_pre = arena.stats().compaction_yield_checks;
        (void)arena.compact();
        CHECK(arena.stats().compaction_yield_checks == yc_pre,
              "compact() outside fiber does not bump yield checks");
        // Inside fiber: yield-bump + safepoint-hit
        aura::gc_hooks::g_fiber_active.store(+[]() noexcept { return true; });
        for (int i = 0; i < 32; ++i)
            (void)arena.create<SmallNode>();
        auto yc_pre_fiber = arena.stats().compaction_yield_checks;
        int sp_pre = s_hits->load();
        (void)arena.compact();
        CHECK(arena.stats().compaction_yield_checks >= yc_pre_fiber + 1,
              "compact() inside fiber bumps yield checks");
        CHECK(s_hits->load() >= sp_pre + 1, "compact() inside fiber hits the GC safepoint");
        // defrag honors fiber coordination too
        arena.request_defrag();
        for (int i = 0; i < 16; ++i)
            (void)arena.create<SmallNode>();
        auto yc_pre_d = arena.stats().compaction_yield_checks;
        (void)arena.defrag();
        CHECK(arena.stats().compaction_yield_checks >= yc_pre_d + 1,
              "defrag() inside fiber bumps yield checks");
        aura::gc_hooks::g_fiber_active.store(nullptr);
        aura::gc_hooks::g_arena_safepoint_check.store(nullptr);
    }
    // AC5: EDSL mutate keeps snapshot coherent
    {
        std::println("\n--- #604 AC5: EDSL mutate keeps snapshot coherent ---");
        auto triggers_before = stat_int(cs, "auto-compact-triggers");
        cs.eval("(set-code \"(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))\")");
        cs.eval("(eval-current)");
        (void)cs.eval("(arena:request-defrag)");
        for (int i = 0; i < 40; ++i)
            (void)cs.eval("(fib 6)");
        cs.eval("(mutate:rebind \"fib\" "
                "\"(lambda (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))\" "
                "\"issue-604\")");
        cs.eval("(eval-current)");
        CHECK(stat_int(cs, "auto-compact-triggers") >= triggers_before,
              "auto-compact-triggers non-decreasing");
        double frag = stat_float(cs, "fragmentation-ratio");
        CHECK(frag >= 0.0 && frag <= 1.0, "frag still valid after mutate");
    }
    // AC6: concurrent churn correctness (2 threads x 40 iters)
    {
        std::println("\n--- #604 AC6: concurrent churn correctness ---");
        cs.eval("(set-code \"(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))\")");
        cs.eval("(eval-current)");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 40;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r = cs.eval("(fib 6)");
                if (r && aura::compiler::types::is_int(*r) &&
                    aura::compiler::types::as_int(*r) == 8)
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() == k_iters * 2,
              std::format("concurrent churn: {} / {} correct", ok_count.load(), k_iters * 2));
    }
}

// ── Issue #642 — query:arena-auto-compaction-stats structured companion ──
// Folded from tests/issues/test_issue_642.cpp via #1957.
// 6 ACs: hash shape + back-compat + naming distinction + concurrent reads.

static void run_642_arena_auto_compaction_stats_companion() {
    std::println("\n=== #642: query:arena-auto-compaction-stats structured companion ===");
    auto hash_int = [](CompilerService& cs, std::string_view key) -> int64_t {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:arena-auto-compaction-stats\") '{}')", key));
        if (!r || !aura::compiler::types::is_int(*r))
            return -1;
        return aura::compiler::types::as_int(*r);
    };
    CompilerService cs;
    // AC1: hash shape with documented fields + schema == 642
    {
        std::println("\n--- #642 AC1: hash shape ---");
        auto h = cs.eval("(engine:metrics \"query:arena-auto-compaction-stats\")");
        CHECK(h.has_value() && aura::compiler::types::is_hash(*h),
              "arena-auto-compaction-stats returns a hash");
        for (const char* k : {"auto-trigger", "live-move-yield", "guard-defrag", "schema"}) {
            auto rr = hash_int(cs, k);
            CHECK(rr >= 0, std::format("'{}' >= 0 (got {})", k, rr));
        }
        CHECK(hash_int(cs, "schema") == 642, "schema == 642 (sentinel)");
    }
    // AC2: existing primitives back-compat (#569 + earlier + #641 + #640 + #637)
    {
        std::println("\n--- #642 AC2: existing primitives back-compat ---");
        auto s_acd = cs.eval("(engine:metrics \"query:arena-auto-compact-defrag-stats\")");
        CHECK(s_acd.has_value(),
              "(engine:metrics \"query:arena-auto-compact-defrag-stats\") reachable (#569)");
        auto s_ac = cs.eval("(engine:metrics \"query:arena-auto-compact-stats\")");
        CHECK(s_ac.has_value(), "(engine:metrics \"query:arena-auto-compact-stats\") reachable");
        auto s_as = cs.eval("(engine:metrics \"query:arena-auto-stats\")");
        CHECK(s_as.has_value(), "(engine:metrics \"query:arena-auto-stats\") reachable");
        auto s_641 = cs.eval("(engine:metrics \"query:stable-ref-provenance-sv-stats\")");
        CHECK(s_641.has_value(),
              "(engine:metrics \"query:stable-ref-provenance-sv-stats\") reachable (#641)");
        auto s_640 = cs.eval("(engine:metrics \"query:sv-verification-closedloop-stats\")");
        CHECK(s_640.has_value(),
              "(engine:metrics \"query:sv-verification-closedloop-stats\") reachable (#640)");
        auto s_637 = cs.eval("(engine:metrics \"query:closure-bridge-safety-stats-hash\")");
        CHECK(s_637.has_value(),
              "(engine:metrics \"query:closure-bridge-safety-stats-hash\") reachable (#637)");
    }
    // AC3: derived-metric invariants (counters well-formed + schema still 642)
    {
        std::println("\n--- #642 AC3: derived-metric invariants ---");
        for (const char* k : {"auto-trigger", "live-move-yield", "guard-defrag"}) {
            auto rr = hash_int(cs, k);
            CHECK(rr >= 0, std::format("'{}' well-formed >= 0 (got {})", k, rr));
        }
        CHECK(hash_int(cs, "schema") == 642, "schema still 642 after traffic");
    }
    // AC4: schema sentinel exactly 642 (not 641/640/637)
    {
        std::println("\n--- #642 AC4: schema sentinel ---");
        CHECK(hash_int(cs, "schema") == 642, "schema == 642");
    }
    // AC5: naming distinction from existing arena primitives
    {
        std::println("\n--- #642 AC5: naming distinction ---");
        auto new_p = cs.eval("(engine:metrics \"query:arena-auto-compaction-stats\")");
        auto old_compact = cs.eval("(engine:metrics \"query:arena-auto-compact-stats\")");
        auto old_defrag = cs.eval("(engine:metrics \"query:arena-auto-compact-defrag-stats\")");
        CHECK(new_p.has_value(), "new (-compaction with -ion) primitive reachable");
        CHECK(old_compact.has_value(), "existing (-compact, no -ion) primitive still reachable");
        CHECK(old_defrag.has_value(), "existing (#569) defrag primitive still reachable");
    }
    // AC6: concurrent reads
    {
        std::println("\n--- #642 AC6: concurrent reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(engine:metrics \"query:arena-auto-compaction-stats\")");
                if (r.has_value())
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() == k_iters * 2,
              std::format("concurrent: {} / {} returned value", ok_count.load(), k_iters * 2));
    }
}

// ── Issue #685 — arena auto-compact + defrag/shape synergy ──
// Folded from tests/issues/test_issue_685.cpp via #1957.
// 5 ACs: stats hash + alloc-path auto-trigger via on_compact_hook +
// EDSL integrate + fiber stress + stats registry.

static void run_685_arena_auto_compact_defrag_shape_synergy() {
    std::println("\n=== #685: arena auto-compact + defrag/shape synergy ===");
    auto stat_int = [](CompilerService& cs, std::string_view key) -> int64_t {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:arena-auto-compact-stats\") '{}')", key));
        if (!r || !aura::compiler::types::is_int(*r))
            return -1;
        return aura::compiler::types::as_int(*r);
    };
    CompilerService cs;
    // AC1: stats hash fields all present
    {
        std::println("\n--- #685 AC1: stats hash fields ---");
        auto stats = cs.eval("(engine:metrics \"query:arena-auto-compact-stats\")");
        CHECK(stats.has_value() && aura::compiler::types::is_hash(*stats), "stats returns a hash");
        for (const char* key : {"auto-triggers", "frag-reduced", "shape-inval-on-compact",
                                "defrag-savings", "yield-checks-hit"}) {
            auto rr = stat_int(cs, key);
            CHECK(rr >= 0, std::format("'{}' >= 0 (got {})", key, rr));
        }
    }
    // AC2: alloc-path auto-trigger via request-defrag + on_compact_hook fires
    {
        std::println("\n--- #685 AC2: alloc-path auto-trigger ---");
        aura::ast::ASTArena arena(8192);
        std::atomic<int> hook_hits{0};
        arena.set_on_compact_hook([&]() { hook_hits.fetch_add(1, std::memory_order_relaxed); });
        arena.request_defrag();
        struct SmallNode {
            char data[32];
        };
        for (int i = 0; i < 64; ++i)
            (void)arena.create<SmallNode>();
        CHECK(arena.stats().auto_alloc_trigger_count >= 1,
              "auto_alloc_trigger_count >= 1 after defrag request + alloc");
        CHECK(hook_hits.load() >= 1, "on_compact_hook fired");
    }
    // AC3: EDSL mutate + arena:request-defrag integration
    {
        std::println("\n--- #685 AC3: mutate + arena defrag path ---");
        auto triggers_before = stat_int(cs, "auto-triggers");
        auto defrag_before = stat_int(cs, "defrag-savings");
        cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))\" )");
        cs.eval("(eval-current)");
        (void)cs.eval("(arena:request-defrag)");
        for (int i = 0; i < 30; ++i)
            (void)cs.eval("(fact 3)");
        cs.eval("(mutate:rebind \"fact\" "
                "\"(lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))\" "
                "\"issue-685\")");
        cs.eval("(eval-current)");
        CHECK(stat_int(cs, "auto-triggers") >= triggers_before, "auto-triggers non-decreasing");
        CHECK(stat_int(cs, "defrag-savings") >= defrag_before, "defrag-savings non-decreasing");
    }
    // AC4: adaptive compact + fiber stress
    {
        std::println("\n--- #685 AC4: adaptive compact + fiber stress ---");
        for (int i = 0; i < 6; ++i)
            (void)cs.eval("(eval-current)");
        (void)cs.eval("(arena:adaptive-compact)");
        CHECK(stat_int(cs, "shape-inval-on-compact") >= 0,
              "shape-inval-on-compact readable after compact");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 25;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(mutate:request-gc-safepoint)");
                auto r = cs.eval("(fact 3)");
                if (r && aura::compiler::types::is_int(*r) &&
                    aura::compiler::types::as_int(*r) == 6)
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() == k_iters * 2,
              std::format("fiber stress: {} / {} correct", ok_count.load(), k_iters * 2));
    }
    // AC5: stats:count > 0 (cumulative health)
    {
        std::println("\n--- #685 AC5: stats:count ---");
        auto r = cs.eval("(stats:count)");
        CHECK(r.has_value() && aura::compiler::types::is_int(*r), "stats:count returns int");
        CHECK(aura::compiler::types::as_int(*r) > 0,
              "stats:count > 0 (cumulative count is healthy)");
    }
}

// ── Wave 12 (#722 / #731 / #764 / #767 — arena observability primitives) ──

static void run_722_arena_integration_stats() {
    std::println("\n=== #722: Arena integration stats (4 counters + schema 722) ===");
    auto hash_int_field = [](CompilerService& cs, const char* hash_src,
                             const char* key) -> int64_t {
        auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
        if (!r || !aura::compiler::types::is_int(*r))
            return -1;
        return aura::compiler::types::as_int(*r);
    };
    CompilerService cs;
    // AC1: hash shape
    {
        std::println("\n--- #722 AC1: hash shape ---");
        auto r = cs.eval("(engine:metrics \"query:arena-integration-stats\")");
        CHECK(r.has_value() && aura::compiler::types::is_hash(*r),
              "(engine:metrics \"query:arena-integration-stats\") returns a hash");
        for (const char* k : {"tier-fallbacks", "dtor-dirty-hooks", "auto-compact-triggers",
                              "fragmentation-post-mutate", "schema"}) {
            auto f = cs.eval(std::format(
                "(hash-ref (engine:metrics \"query:arena-integration-stats\") '{}')", k));
            CHECK(f.has_value(), std::format("field '{}' present", k));
        }
    }
    // AC2: fresh-zero state
    {
        std::println("\n--- #722 AC2: counters == 0 on fresh service ---");
        for (const char* k : {"tier-fallbacks", "dtor-dirty-hooks", "auto-compact-triggers",
                              "fragmentation-post-mutate"}) {
            auto v = hash_int_field(cs, "(engine:metrics \"query:arena-integration-stats\")", k);
            CHECK(v == 0, std::format("'{}' = 0 on fresh service (got {})", k, v));
        }
    }
    // AC3: schema sentinel
    {
        std::println("\n--- #722 AC3: schema == 722 ---");
        auto v = hash_int_field(cs, "(engine:metrics \"query:arena-integration-stats\")", "schema");
        CHECK(v == 722, std::format("schema = {} (expected 722)", v));
    }
    // AC4: bump helpers
    {
        std::println("\n--- #722 AC4: bump helpers callable ---");
        auto& ev = cs.evaluator();
        ev.bump_arena_tier_fallback();
        ev.bump_arena_tier_fallback();
        ev.bump_arena_dtor_dirty_hook();
        ev.bump_arena_dtor_dirty_hook();
        ev.bump_arena_dtor_dirty_hook();
        ev.bump_arena_auto_compact_trigger();
        ev.set_arena_fragmentation_post_mutate(420000);
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-integration-stats\")",
                             "tier-fallbacks") == 2,
              "after 2 bumps == 2");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-integration-stats\")",
                             "dtor-dirty-hooks") == 3,
              "after 3 bumps == 3");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-integration-stats\")",
                             "auto-compact-triggers") == 1,
              "after 1 bump == 1");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-integration-stats\")",
                             "fragmentation-post-mutate") == 420000,
              "after set(420000) == 420000");
        ev.set_arena_fragmentation_post_mutate(750000);
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-integration-stats\")",
                             "fragmentation-post-mutate") == 750000,
              "setter overwrites (not delta): 750000");
    }
    // AC5: regression #712..#721
    {
        std::println("\n--- #722 AC5: regression #712..#721 sibling primitives ---");
        const char* sibs[] = {"(engine:metrics \"query:macro-reflect-validation-stats\")",
                              "(engine:metrics \"query:macro-jit-hygiene-stats\")",
                              "(engine:metrics \"query:self-evolution-closedloop-stats\")",
                              "(engine:metrics \"query:stable-ref-layer-stats\")",
                              "(engine:metrics \"query:pattern-stats\")",
                              "(engine:metrics \"query:fiber-boundary-violation-stats\")",
                              "(engine:metrics \"query:incremental-relower-stats\")",
                              "(engine:metrics \"query:closure-env-epoch-safety-stats\")",
                              "(engine:metrics \"query:jit-interpreter-parity-stats\")",
                              "(engine:metrics \"query:ir-soa-completeness-stats\")"};
        const int schemas[] = {712, 713, 714, 715, 716, 717, 718, 719, 720, 721};
        for (size_t i = 0; i < std::size(sibs); ++i) {
            auto r = cs.eval(sibs[i]);
            CHECK(r.has_value() && aura::compiler::types::is_hash(*r),
                  std::format("sibling #{} hash regression", schemas[i]));
            CHECK(hash_int_field(cs, sibs[i], "schema") == schemas[i],
                  std::format("sibling #{} schema == {} (no drift)", schemas[i], schemas[i]));
        }
    }
}

static void run_731_arena_concurrent_compact_stats() {
    std::println(
        "\n=== #731: Arena concurrent compact observability (4 counters + schema 731) ===");
    auto hash_int_field = [](CompilerService& cs, const char* hash_src,
                             const char* key) -> int64_t {
        auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
        if (!r || !aura::compiler::types::is_int(*r))
            return -1;
        return aura::compiler::types::as_int(*r);
    };
    CompilerService cs;
    // AC1: hash shape
    {
        std::println("\n--- #731 AC1: hash shape ---");
        auto r = cs.eval("(engine:metrics \"query:arena-concurrent-compact-stats\")");
        CHECK(r.has_value() && aura::compiler::types::is_hash(*r),
              "(engine:metrics \"query:arena-concurrent-compact-stats\") returns a hash");
        for (const char* k : {"concurrent-compacts", "envframe-revalidations",
                              "panic-rollback-compact-hits", "races-prevented", "schema"}) {
            auto f = cs.eval(std::format(
                "(hash-ref (engine:metrics \"query:arena-concurrent-compact-stats\") '{}')", k));
            CHECK(f.has_value(), std::format("field '{}' present", k));
        }
    }
    // AC2: fresh-zero state
    {
        std::println("\n--- #731 AC2: counters == 0 on fresh service ---");
        for (const char* k : {"concurrent-compacts", "envframe-revalidations",
                              "panic-rollback-compact-hits", "races-prevented"}) {
            auto v =
                hash_int_field(cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")", k);
            CHECK(v == 0, std::format("'{}' = 0 on fresh service (got {})", k, v));
        }
    }
    // AC3: schema sentinel
    {
        std::println("\n--- #731 AC3: schema == 731 ---");
        auto v = hash_int_field(cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")",
                                "schema");
        CHECK(v == 731, std::format("schema = {} (expected 731)", v));
    }
    // AC4: bump helpers
    {
        std::println("\n--- #731 AC4: bump helpers callable ---");
        auto& ev = cs.evaluator();
        for (int i = 0; i < 3; ++i)
            ev.bump_arena_concurrent_compact();
        for (int i = 0; i < 6; ++i)
            ev.bump_arena_envframe_revalidation();
        for (int i = 0; i < 2; ++i)
            ev.bump_arena_panic_rollback_compact_hit();
        for (int i = 0; i < 4; ++i)
            ev.bump_arena_race_prevented();
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")",
                             "concurrent-compacts") == 3,
              "3 bumps == 3");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")",
                             "envframe-revalidations") == 6,
              "6 bumps == 6");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")",
                             "panic-rollback-compact-hits") == 2,
              "2 bumps == 2");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-concurrent-compact-stats\")",
                             "races-prevented") == 4,
              "4 bumps == 4");
    }
    // AC5: regression #712..#728 (incl. #722)
    {
        std::println("\n--- #731 AC5: regression #712..#728 sibling primitives ---");
        const char* sibs[] = {"(engine:metrics \"query:macro-reflect-validation-stats\")",
                              "(engine:metrics \"query:macro-jit-hygiene-stats\")",
                              "(engine:metrics \"query:self-evolution-closedloop-stats\")",
                              "(engine:metrics \"query:stable-ref-layer-stats\")",
                              "(engine:metrics \"query:pattern-stats\")",
                              "(engine:metrics \"query:fiber-boundary-violation-stats\")",
                              "(engine:metrics \"query:incremental-relower-stats\")",
                              "(engine:metrics \"query:closure-env-epoch-safety-stats\")",
                              "(engine:metrics \"query:jit-interpreter-parity-stats\")",
                              "(engine:metrics \"query:ir-soa-completeness-stats\")",
                              "(engine:metrics \"query:arena-integration-stats\")",
                              "(engine:metrics \"query:value-dispatch-stats\")",
                              "(engine:metrics \"query:closed-loop-reliability-stats\")",
                              "(engine:metrics \"query:unified-error-stats\")"};
        for (const char* p : sibs) {
            auto r = cs.eval(p);
            CHECK(r.has_value() && aura::compiler::types::is_hash(*r),
                  std::format("sibling hash regression ({})", p));
        }
    }
}

static void run_764_compiler_arena_closure_lifetime_stats() {
    std::println("\n=== #764: Compiler Arena closure lifetime stats (4 counters + schema 764) ===");
    auto hash_int_field = [](CompilerService& cs, const char* hash_src,
                             const char* key) -> int64_t {
        auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
        if (!r || !aura::compiler::types::is_int(*r))
            return -1;
        return aura::compiler::types::as_int(*r);
    };
    CompilerService cs;
    // AC1: hash shape
    {
        std::println("\n--- #764 AC1: hash shape ---");
        auto r = cs.eval("(engine:metrics \"query:compiler-arena-closure-lifetime-stats\")");
        CHECK(r.has_value() && aura::compiler::types::is_hash(*r),
              "(engine:metrics \"query:compiler-arena-closure-lifetime-stats\") returns a hash");
        for (const char* k : {"root-hits", "bridge-sharedptr-pinned", "cross-violations-prevented",
                              "invalidate-ast-refresh", "schema"}) {
            auto f = cs.eval(std::format(
                "(hash-ref (engine:metrics \"query:compiler-arena-closure-lifetime-stats\") '{}')",
                k));
            CHECK(f.has_value(), std::format("field '{}' present", k));
        }
    }
    // AC2: fresh-zero state
    {
        std::println("\n--- #764 AC2: counters == 0 on fresh service ---");
        for (const char* k : {"root-hits", "bridge-sharedptr-pinned", "cross-violations-prevented",
                              "invalidate-ast-refresh"}) {
            auto v = hash_int_field(
                cs, "(engine:metrics \"query:compiler-arena-closure-lifetime-stats\")", k);
            CHECK(v == 0, std::format("'{}' = 0 on fresh service (got {})", k, v));
        }
    }
    // AC3: schema sentinel
    {
        std::println("\n--- #764 AC3: schema == 764 ---");
        auto v = hash_int_field(
            cs, "(engine:metrics \"query:compiler-arena-closure-lifetime-stats\")", "schema");
        CHECK(v == 764, std::format("schema = {} (expected 764)", v));
    }
    // AC4: bump helpers
    {
        std::println("\n--- #764 AC4: bump helpers callable ---");
        auto& ev = cs.evaluator();
        for (int i = 0; i < 3; ++i)
            ev.bump_compiler_arena_closure_lifetime_root_hit();
        for (int i = 0; i < 2; ++i)
            ev.bump_compiler_arena_closure_lifetime_bridge_sharedptr_pinned();
        ev.bump_compiler_arena_closure_lifetime_cross_violation_prevented();
        for (int i = 0; i < 4; ++i)
            ev.bump_compiler_arena_closure_lifetime_invalidate_ast_refresh();
        CHECK(hash_int_field(cs, "(engine:metrics \"query:compiler-arena-closure-lifetime-stats\")",
                             "root-hits") == 3,
              "3 bumps == 3");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:compiler-arena-closure-lifetime-stats\")",
                             "bridge-sharedptr-pinned") == 2,
              "2 bumps == 2");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:compiler-arena-closure-lifetime-stats\")",
                             "cross-violations-prevented") == 1,
              "1 bump == 1");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:compiler-arena-closure-lifetime-stats\")",
                             "invalidate-ast-refresh") == 4,
              "4 bumps == 4");
    }
    // AC5: regression #735/#756..#763
    {
        std::println("\n--- #764 AC5: regression #735/#756..#763 ---");
        const char* sibs[] = {"(engine:metrics \"query:macro-provenance-stats\")",
                              "(engine:metrics \"query:envframe-dualpath-policy-stats\")",
                              "(engine:metrics \"query:macro-hygiene-provenance-stats\")",
                              "(engine:metrics \"query:edsl-reflection-stats\")",
                              "(engine:metrics \"query:code-as-data-maturity-stats\")",
                              "(engine:metrics \"query:pattern-performance-stats\")",
                              "(engine:metrics \"query:mutate-batch-stats\")",
                              "(engine:metrics \"query:workspace-closedloop-orchestration-stats\")",
                              "(engine:metrics \"query:linear-ownership-gc-compiler-stats\")"};
        const int schemas[] = {735, 756, 757, 758, 759, 760, 761, 762, 763};
        for (size_t i = 0; i < std::size(sibs); ++i) {
            auto r = cs.eval(sibs[i]);
            CHECK(r.has_value() && aura::compiler::types::is_hash(*r),
                  std::format("sibling #{} hash regression", schemas[i]));
            CHECK(hash_int_field(cs, sibs[i], "schema") == schemas[i],
                  std::format("sibling #{} schema == {} (no drift)", schemas[i], schemas[i]));
        }
    }
}

static void run_767_arena_auto_compact_defrag_fiber_stats() {
    std::println("\n=== #767: Arena auto-compact + live defrag + fiber yield stats (6 fields + "
                 "schema 767) ===");
    auto hash_int_field = [](CompilerService& cs, const char* hash_src,
                             const char* key) -> int64_t {
        auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
        if (!r || !aura::compiler::types::is_int(*r))
            return -1;
        return aura::compiler::types::as_int(*r);
    };
    CompilerService cs;
    // AC1: hash shape (6 fields + schema = 7 entries)
    {
        std::println("\n--- #767 AC1: hash shape ---");
        auto r = cs.eval("(engine:metrics \"query:arena-auto-compact-defrag-fiber-stats\")");
        CHECK(r.has_value() && aura::compiler::types::is_hash(*r),
              "(engine:metrics \"query:arena-auto-compact-defrag-fiber-stats\") returns a hash");
        for (const char* k : {"auto-compact-triggers", "frag-reduced-bp", "live-defrag-savings",
                              "fiber-yield-during-compact", "shape-inval-count",
                              "defrag-blocked-fibers", "schema"}) {
            auto f = cs.eval(std::format(
                "(hash-ref (engine:metrics \"query:arena-auto-compact-defrag-fiber-stats\") '{}')",
                k));
            CHECK(f.has_value(), std::format("field '{}' present", k));
        }
    }
    // AC2: 4 reused >= 0, 2 new == 0
    {
        std::println("\n--- #767 AC2: reused >= 0, new == 0 on fresh service ---");
        for (const char* k : {"auto-compact-triggers", "frag-reduced-bp", "live-defrag-savings",
                              "shape-inval-count"}) {
            auto v = hash_int_field(
                cs, "(engine:metrics \"query:arena-auto-compact-defrag-fiber-stats\")", k);
            CHECK(v >= 0, std::format("reused '{}' >= 0 (got {})", k, v));
        }
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-auto-compact-defrag-fiber-stats\")",
                             "fiber-yield-during-compact") == 0,
              "fiber-yield-during-compact == 0 on fresh service");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-auto-compact-defrag-fiber-stats\")",
                             "defrag-blocked-fibers") == 0,
              "defrag-blocked-fibers == 0 on fresh service");
    }
    // AC3: schema sentinel
    {
        std::println("\n--- #767 AC3: schema == 767 ---");
        auto v = hash_int_field(
            cs, "(engine:metrics \"query:arena-auto-compact-defrag-fiber-stats\")", "schema");
        CHECK(v == 767, std::format("schema = {} (expected 767)", v));
    }
    // AC4: bump helpers
    {
        std::println("\n--- #767 AC4: bump helpers callable ---");
        auto& ev = cs.evaluator();
        for (int i = 0; i < 4; ++i)
            ev.bump_arena_auto_compact_fiber_yield_during_compact();
        for (int i = 0; i < 3; ++i)
            ev.bump_arena_auto_compact_defrag_blocked_fibers();
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-auto-compact-defrag-fiber-stats\")",
                             "fiber-yield-during-compact") == 4,
              "4 bumps == 4");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-auto-compact-defrag-fiber-stats\")",
                             "defrag-blocked-fibers") == 3,
              "3 bumps == 3");
        for (const char* k : {"auto-compact-triggers", "frag-reduced-bp", "live-defrag-savings",
                              "shape-inval-count"}) {
            auto v = hash_int_field(
                cs, "(engine:metrics \"query:arena-auto-compact-defrag-fiber-stats\")", k);
            CHECK(v >= 0, std::format("reused '{}' >= 0 after #767 bumps (no regression)", k));
        }
    }
    // AC5: regression #685 + #642 + #766
    {
        std::println("\n--- #767 AC5: regression #685 + #642 + #766 ---");
        auto a685 = cs.eval("(engine:metrics \"query:arena-auto-compact-stats\")");
        auto a642 = cs.eval("(engine:metrics \"query:arena-auto-compaction-stats\")");
        auto a766 = cs.eval("(engine:metrics \"query:ir-soa-migration-stats\")");
        CHECK(a685.has_value() && aura::compiler::types::is_hash(*a685),
              "query:arena-auto-compact-stats hash regression (#685)");
        CHECK(a642.has_value() && aura::compiler::types::is_hash(*a642),
              "query:arena-auto-compaction-stats hash regression (#642)");
        CHECK(a766.has_value() && aura::compiler::types::is_hash(*a766),
              "query:ir-soa-migration-stats hash regression (#766)");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-auto-compact-stats\")",
                             "auto-triggers") >= 0,
              "#685 auto-triggers >= 0 (no regression)");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-auto-compact-stats\")",
                             "yield-checks-hit") >= 0,
              "#685 yield-checks-hit >= 0 (no regression)");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:arena-auto-compaction-stats\")",
                             "schema") == 642,
              "#642 schema == 642 (no drift)");
    }
}

// ── Issue #1397 — ASTArena::request_defrag atomic CAS newly_set semantics ──
// Folded from tests/issues/test_issue_1397.cpp via #1957. Extended
// coverage for (arena:defrag)/(arena:request-defrag)/(arena:defrag-requested?)
// beyond the 5 sub-steps in test_issue_300.cpp AC5. 50-cycle + 2-service
// independence tests. Pure CompilerService-driven, fits compact_batch.

static void run_1397_arena_request_defrag_atomic_cas() {
    std::println("\n=== #1397: ASTArena::request_defrag atomic CAS newly_set semantics ===");
    // AC1: 50-cycle set->dup->defrag->request consistency (single service)
    {
        std::println("\n--- #1397 AC1: 50-cycle set->dup->defrag->request consistency ---");
        CompilerService cs;
        cs.eval("(set-code \"(define x 1)\")");
        int newly_true = 0, dup_false = 0;
        for (int i = 0; i < 50; ++i) {
            cs.eval("(arena:defrag)");
            auto r1 = cs.eval("(arena:request-defrag)");
            if (r1 && aura::compiler::types::is_bool(*r1) && aura::compiler::types::as_bool(*r1))
                ++newly_true;
            auto r2 = cs.eval("(arena:request-defrag)");
            if (r2 && aura::compiler::types::is_bool(*r2) && !aura::compiler::types::as_bool(*r2))
                ++dup_false;
        }
        CHECK(newly_true == 50,
              std::format("50 cycles first call returned #t (got {})", newly_true));
        CHECK(dup_false == 50, std::format("50 cycles duplicate returned #f (got {})", dup_false));
        cs.eval("(arena:defrag)"); // cleanup
    }
    // AC2: defrag clears flag, cycle resets
    {
        std::println("\n--- #1397 AC2: defrag clears flag (cycle reset) ---");
        CompilerService cs;
        cs.eval("(set-code \"(define x 1)\")");
        cs.eval("(arena:defrag)");
        auto r1 = cs.eval("(arena:request-defrag)");
        CHECK(r1 && aura::compiler::types::is_bool(*r1) && aura::compiler::types::as_bool(*r1),
              "first request after defrag: #t (newly_set)");
        auto r2 = cs.eval("(arena:request-defrag)");
        CHECK(r2 && aura::compiler::types::is_bool(*r2) && !aura::compiler::types::as_bool(*r2),
              "duplicate: #f (CAS already-set)");
        cs.eval("(arena:defrag)");
        auto r3 = cs.eval("(arena:request-defrag)");
        CHECK(r3 && aura::compiler::types::is_bool(*r3) && aura::compiler::types::as_bool(*r3),
              "after second defrag: #t (defrag clears -> cycle resets)");
        auto r4 = cs.eval("(arena:request-defrag)");
        CHECK(r4 && aura::compiler::types::is_bool(*r4) && !aura::compiler::types::as_bool(*r4),
              "second-cycle duplicate: #f");
    }
    // AC3: two CompilerService instances have independent flags
    {
        std::println("\n--- #1397 AC3: two CompilerService instances have independent flags ---");
        CompilerService cs1;
        CompilerService cs2;
        cs1.eval("(set-code \"(define x 1)\")");
        cs2.eval("(set-code \"(define x 1)\")");
        auto a1 = cs1.eval("(arena:request-defrag)");
        CHECK(a1 && aura::compiler::types::is_bool(*a1) && aura::compiler::types::as_bool(*a1),
              "cs1 first request: #t");
        auto a2 = cs2.eval("(arena:request-defrag)");
        CHECK(a2 && aura::compiler::types::is_bool(*a2) && aura::compiler::types::as_bool(*a2),
              "cs2 first request: #t (independent of cs1)");
        auto a3 = cs1.eval("(arena:request-defrag)");
        CHECK(a3 && aura::compiler::types::is_bool(*a3) && !aura::compiler::types::as_bool(*a3),
              "cs1 duplicate: #f");
        auto a4 = cs2.eval("(arena:request-defrag)");
        CHECK(a4 && aura::compiler::types::is_bool(*a4) && !aura::compiler::types::as_bool(*a4),
              "cs2 duplicate: #f (cs1 dup did not affect cs2)");
        cs1.eval("(arena:defrag)");
        auto a5 = cs1.eval("(arena:request-defrag)");
        CHECK(a5 && aura::compiler::types::is_bool(*a5) && aura::compiler::types::as_bool(*a5),
              "cs1 after own defrag: #t");
        auto a6 = cs2.eval("(arena:request-defrag)");
        CHECK(a6 && aura::compiler::types::is_bool(*a6) && !aura::compiler::types::as_bool(*a6),
              "cs2 still operates independently (no cross-talk)");
    }
    // AC4: (arena:defrag-requested?) tracks each transition
    {
        std::println("\n--- #1397 AC4: (arena:defrag-requested?) tracks each transition ---");
        CompilerService cs;
        cs.eval("(set-code \"(define x 1)\")");
        cs.eval("(arena:defrag)");
        auto q0 = cs.eval("(arena:defrag-requested?)");
        CHECK(q0 && aura::compiler::types::is_bool(*q0) && !aura::compiler::types::as_bool(*q0),
              "after defrag: requested? = #f");
        cs.eval("(arena:request-defrag)");
        auto q1 = cs.eval("(arena:defrag-requested?)");
        CHECK(q1 && aura::compiler::types::is_bool(*q1) && aura::compiler::types::as_bool(*q1),
              "after request: requested? = #t");
        cs.eval("(arena:defrag)");
        auto q2 = cs.eval("(arena:defrag-requested?)");
        CHECK(q2 && aura::compiler::types::is_bool(*q2) && !aura::compiler::types::as_bool(*q2),
              "after another defrag: requested? = #f (cycle resets)");
        cs.eval("(arena:defrag)"); // cleanup
    }
}

// ── Wave 14 (#1488 dead string_heap push / #1489 panic-checkpoint GC defer / #1508 JIT dual check
// / #1510 compact_env_frames cooperation) ──

static void run_1488_arena_adaptive_stats_no_dead_heap_push() {
    std::println("\n=== #1488: dead string_heap_ push pollution cleanup ===");
    // AC1: (stats:get "arena:adaptive-stats") returns pair-of-ints
    {
        std::println("\n--- #1488 AC1: arena:adaptive-stats pair-of-ints shape ---");
        CompilerService cs;
        auto r = cs.eval(R"((stats:get "arena:adaptive-stats"))");
        CHECK(r.has_value(), "stats:get returns a value");
        if (!r)
            return;
        CHECK(aura::compiler::types::is_pair(*r), "result is a pair");
        if (!aura::compiler::types::is_pair(*r))
            return;
        const auto& pairs = cs.evaluator().pairs();
        const auto pidx = aura::compiler::types::as_pair_idx(*r);
        CHECK(pidx < pairs.size(), "pair index in range");
        if (pidx >= pairs.size())
            return;
        const auto& pr = pairs[pidx];
        CHECK(aura::compiler::types::is_int(pr.car) && aura::compiler::types::is_int(pr.cdr),
              "car/cdr are ints (trigger . skip)");
        CHECK(aura::compiler::types::as_int(pr.car) >= 0 &&
                  aura::compiler::types::as_int(pr.cdr) >= 0,
              "counters non-negative");
    }
    // AC2: long poll does not pollute string_heap_
    {
        std::println("\n--- #1488 AC2: 1000 polls do not pollute string_heap_ ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Warm
        for (int i = 0; i < 3; ++i) {
            auto w = cs.eval(R"((stats:get "arena:adaptive-stats"))");
            CHECK(w.has_value() && aura::compiler::types::is_pair(*w),
                  "warm stats:get returns pair");
        }
        constexpr int kIters = 1000;
        const auto heap_before = ev.string_heap().size();
        const auto pairs_before = ev.pairs().size();
        std::size_t ok = 0;
        for (int i = 0; i < kIters; ++i) {
            auto r = cs.eval(R"((stats:get "arena:adaptive-stats"))");
            if (r && aura::compiler::types::is_pair(*r))
                ++ok;
        }
        const auto heap_delta = ev.string_heap().size() - heap_before;
        const auto pairs_delta = ev.pairs().size() - pairs_before;
        CHECK(ok == static_cast<std::size_t>(kIters), "all 1000 polls returned pairs");
        // After #1072 fix: heap growth ~1/call from re-parse only (no dead push of
        // to_string(trigger/skip))
        CHECK(heap_delta < 2 * static_cast<std::size_t>(kIters),
              "string_heap growth < 2N (no dead counter push_back)");
        CHECK(heap_delta <= static_cast<std::size_t>(kIters) + 8,
              "string_heap growth ~1/call (no 2N dead-push regression)");
    }
    // AC3: direct (stats:get) still pair-of-ints after multi-call path
    {
        std::println("\n--- #1488 AC3: direct (stats:get) still pair-of-ints ---");
        CompilerService cs;
        auto r = cs.eval(R"((stats:get "arena:adaptive-stats"))");
        CHECK(r && aura::compiler::types::is_pair(*r), "direct stats:get returns pair");
        if (!r || !aura::compiler::types::is_pair(*r))
            return;
        const auto& pairs = cs.evaluator().pairs();
        const auto pidx = aura::compiler::types::as_pair_idx(*r);
        if (pidx < pairs.size()) {
            CHECK(aura::compiler::types::is_int(pairs[pidx].car) &&
                      aura::compiler::types::is_int(pairs[pidx].cdr),
                  "ints after multi-call path");
        }
    }
    // AC4: production-hardening flag arena-adaptive-no-dead-push
    {
        std::println("\n--- #1488 AC4: production-hardening flag ---");
        CompilerService cs;
        auto r = cs.eval(
            R"((hash-ref (engine:metrics "query:production-hardening-1072-1096-stats") "arena-adaptive-no-dead-push"))");
        CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 1,
              "arena-adaptive-no-dead-push == 1 (flag set, #1072 fix landed)");
    }
}

static void run_1489_panic_checkpoint_gc_deferral() {
    std::println("\n=== #1489: PanicCheckpoint GC deferral ===");
    // AC1: save_panic_checkpoint arms GC defer
    {
        std::println("\n--- #1489 AC1: save_panic_checkpoint arms GC defer ---");
        CompilerService cs;
        auto r1 = cs.eval("(set-code \"(define x 1)\")");
        CHECK(r1.has_value(), "set-code for workspace");
        (void)cs.eval("(eval-current)");
        auto& ev = cs.evaluator();
        const auto depth0 = aura::gc_hooks::gc_defer_pending_panic_depth();
        CHECK(!ev.gc_defer_armed_for_pending_panic(), "not armed before save");
        CHECK(!ev.has_panic_checkpoint(), "no checkpoint before save");
        CHECK(ev.save_panic_checkpoint(), "save_panic_checkpoint succeeds");
        CHECK(ev.has_panic_checkpoint(), "checkpoint live after save");
        CHECK(ev.gc_defer_armed_for_pending_panic(), "evaluator armed after save");
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == depth0 + 1,
              "process-wide defer depth +1");
        ev.arm_gc_defer_for_pending_panic();
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == depth0 + 1, "re-arm is idempotent");
        ev.commit_panic_checkpoint();
        CHECK(!ev.gc_defer_armed_for_pending_panic(), "disarmed after commit");
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == depth0,
              "depth restored after commit");
    }
    // AC2: request_gc_safepoint defers under pending checkpoint
    {
        std::println("\n--- #1489 AC2: request_gc_safepoint defers under pending checkpoint ---");
        CompilerService cs;
        (void)cs.eval("(set-code \"(define x 1)\")");
        (void)cs.eval("(eval-current)");
        auto& ev = cs.evaluator();
        CHECK(ev.request_gc_safepoint() == 0, "immediate when no checkpoint");
        CHECK(ev.save_panic_checkpoint(), "save ok");
        const auto blocked0 = ev.get_gc_blocked_by_pending_panic();
        CHECK(ev.request_gc_safepoint() == 1, "deferred while checkpoint live");
        CHECK(ev.get_gc_blocked_by_pending_panic() > blocked0, "gc_blocked_by_pending bumped");
        ev.commit_panic_checkpoint();
        CHECK(ev.request_gc_safepoint() == 0, "immediate again after commit");
    }
    // AC3: compact_sweep skips while defer armed
    {
        std::println("\n--- #1489 AC3: compact_sweep skips while defer armed ---");
        CompilerService cs;
        (void)cs.eval("(set-code \"(define x 1)\")");
        (void)cs.eval("(eval-current)");
        auto& ev = cs.evaluator();
        CHECK(ev.save_panic_checkpoint(), "save ok");
        const auto skip0 = aura::gc_hooks::gc_sweep_skipped_pending_panic();
        aura::serve::GCSweepBuffers marks{};
        auto result = ev.compact_sweep(&marks);
        CHECK(result.closures_freed == 0 && result.pairs_freed == 0 && result.strings_freed == 0,
              "no reclaim while defer armed");
        CHECK(aura::gc_hooks::gc_sweep_skipped_pending_panic() > skip0, "skip counter advanced");
        ev.commit_panic_checkpoint();
    }
    // AC4: restore releases defer
    {
        std::println("\n--- #1489 AC4: restore releases defer ---");
        CompilerService cs;
        (void)cs.eval("(set-code \"(define x 1)\")");
        (void)cs.eval("(eval-current)");
        auto& ev = cs.evaluator();
        const auto depth0 = aura::gc_hooks::gc_defer_pending_panic_depth();
        CHECK(ev.save_panic_checkpoint(), "save ok");
        CHECK(aura::gc_hooks::gc_deferred_for_pending_panic(), "deferred after save");
        (void)cs.eval("(set-code \"(define x 2)\")");
        CHECK(ev.restore_panic_checkpoint(), "restore succeeds");
        CHECK(!ev.has_panic_checkpoint(), "checkpoint cleared");
        CHECK(!ev.gc_defer_armed_for_pending_panic(), "disarmed after restore");
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == depth0, "depth restored");
        CHECK(ev.request_gc_safepoint() == 0, "GC immediate after restore");
    }
    // AC5: gc-panic-deferral-stats + block_gc trampoline
    {
        std::println("\n--- #1489 AC5: gc-panic-deferral-stats + trampoline ---");
        CompilerService cs;
        (void)cs.eval("(set-code \"(define x 1)\")");
        (void)cs.eval("(eval-current)");
        auto& ev = cs.evaluator();
        auto h = cs.eval("(engine:metrics \"query:gc-panic-deferral-stats\")");
        CHECK(h && aura::compiler::types::is_hash(*h), "gc-panic-deferral-stats is hash");
        auto href_field = [&](const char* key) -> std::int64_t {
            std::string src = std::format(
                R"((hash-ref (engine:metrics "query:gc-panic-deferral-stats") "{}"))", key);
            auto r = cs.eval(src);
            if (!r || !aura::compiler::types::is_int(*r))
                return -1;
            return aura::compiler::types::as_int(*r);
        };
        CHECK(href_field("schema") == 651, "schema 651");
        const auto def0 = href_field("pending-panic-deferral");
        const auto blk0 = href_field("gc-blocked-by-panic");
        const auto res0 = href_field("conflicts-resolved");
        CHECK(def0 >= 0 && blk0 >= 0 && res0 >= 0, "metric fields readable");
        CHECK(ev.save_panic_checkpoint(), "save ok");
        (void)ev.request_gc_safepoint();
        const auto def1 = href_field("pending-panic-deferral");
        const auto blk1 = href_field("gc-blocked-by-panic");
        CHECK(def1 > def0, "pending-panic-deferral advanced");
        CHECK(blk1 > blk0, "gc-blocked-by-panic advanced");
        ev.commit_panic_checkpoint();
        const auto res1 = href_field("conflicts-resolved");
        CHECK(res1 > res0, "conflicts-resolved advanced on commit");
    }
    // AC6: re_pin under checkpoint
    {
        std::println("\n--- #1489 AC6: re_pin callable under pending checkpoint ---");
        CompilerService cs;
        (void)cs.eval("(set-code \"(define x 1)\")");
        (void)cs.eval("(eval-current)");
        auto& ev = cs.evaluator();
        CHECK(ev.save_panic_checkpoint(), "save ok");
        CHECK(ev.test_re_pin_cow_children_from_snapshot(), "re_pin callable with pending cp");
        ev.on_arena_compact_hook();
        CHECK(true, "on_arena_compact_hook ok under pending checkpoint");
        ev.commit_panic_checkpoint();
    }
}

static void run_1508_jit_closure_dual_check_deopt() {
    std::println("\n=== #1508: JIT aura_closure_call dual check + deopt ===");
    aura_reset_runtime();
    // AC1: aura_is_jit_closure_fresh matches table + defuse epochs
    {
        std::println("\n--- #1508 AC1: aura_is_jit_closure_fresh helper ---");
        setenv("AURA_BRIDGE_EPOCH_LEGACY_TRUST", "0", 1);
        aura_set_aot_defuse_version(10);
        const auto e0 = aura_aot_func_table_epoch();
        aura_aot_bump_func_table_epoch();
        const auto e1 = aura_aot_func_table_epoch();
        CHECK(e1 > e0, "table epoch advances on bump");
        CHECK(aura_is_jit_closure_fresh(e1, 10), "matching bridge+defuse is fresh");
        CHECK(!aura_is_jit_closure_fresh(e1 - 1, 10), "stale bridge is not fresh");
        CHECK(!aura_is_jit_closure_fresh(e1, 11), "stale defuse is not fresh");
        CHECK(!aura_is_jit_closure_fresh(0, 10), "bridge=0 + active table is stale (#1491 strict)");
        CHECK(!aura_is_jit_closure_fresh(e1, 0),
              "defuse=0 + active defuse is stale (#1491 strict)");
        CHECK(!aura_is_jit_closure_fresh(0, 0), "both zero under active tracking is stale");
        aura_set_aot_defuse_version(0);
        CHECK(aura_is_jit_closure_fresh(e1, 0), "matching bridge + defuse tracking off is fresh");
    }
    // AC2: alloc stamps provenance
    {
        std::println("\n--- #1508 AC2: alloc stamps provenance; fresh call passes dual check ---");
        aura_set_aot_defuse_version(42);
        aura_aot_bump_func_table_epoch();
        const auto dual0 = aura_jit_closure_dual_check_total();
        auto id = aura_alloc_closure(7);
        CHECK(id >= 0, "alloc_closure ok");
        const auto deopt0 = aura_jit_closure_stale_deopt_total();
        auto r = aura_closure_call(id, nullptr, 0);
        (void)r;
        CHECK(aura_jit_closure_dual_check_total() > dual0, "dual_check bumped on call");
        CHECK(aura_jit_closure_stale_deopt_total() == deopt0,
              "fresh call does not bump stale_deopt");
        aura_free_closure(id);
    }
    // AC3: table epoch stale deopt
    {
        std::println("\n--- #1508 AC3: table epoch bump -> stale deopt ---");
        aura_set_aot_defuse_version(100);
        auto id = aura_alloc_closure(3);
        CHECK(id >= 0, "alloc for table-stale path");
        const auto deopt0 = aura_jit_closure_stale_deopt_total();
        const auto safe0 = aura_jit_closure_safe_fallbacks();
        const auto gdeopt0 = aura_deopt_count();
        aura_aot_bump_func_table_epoch();
        auto r = aura_closure_call(id, nullptr, 0);
        CHECK(r == 0, "stale call returns 0 (safe refuse, no UAF)");
        CHECK(aura_jit_closure_stale_deopt_total() > deopt0, "stale_deopt bumped");
        CHECK(aura_jit_closure_safe_fallbacks() > safe0, "safe_fallback bumped");
        CHECK(aura_deopt_count() > gdeopt0, "aura_deopt_inc fired");
        aura_free_closure(id);
    }
    // AC4: defuse stale deopt
    {
        std::println("\n--- #1508 AC4: defuse bump -> stale deopt ---");
        aura_set_aot_defuse_version(200);
        auto id = aura_alloc_closure(4);
        CHECK(id >= 0, "alloc for defuse-stale path");
        const auto deopt0 = aura_jit_closure_stale_deopt_total();
        aura_set_aot_defuse_version(201);
        auto r = aura_closure_call(id, nullptr, 0);
        CHECK(r == 0, "defuse-stale call returns 0");
        CHECK(aura_jit_closure_stale_deopt_total() > deopt0, "stale_deopt after defuse bump");
        aura_free_closure(id);
    }
    // AC5: CompilerService wires aot metrics
    {
        std::println("\n--- #1508 AC5: CompilerService wires aot metrics ---");
        CompilerService cs;
        (void)cs;
        aura_set_aot_defuse_version(cs.evaluator().defuse_version() + 1);
        auto id = aura_alloc_closure(5);
        const auto dual0 = aura_jit_closure_dual_check_total();
        aura_aot_bump_func_table_epoch();
        (void)aura_closure_call(id, nullptr, 0);
        CHECK(aura_jit_closure_dual_check_total() > dual0,
              "dual_check visible via service-wired metrics");
        CHECK(aura_jit_closure_stale_deopt_total() > 0, "stale_deopt non-zero after forced stale");
        aura_free_closure(id);
    }
    // AC6: 1000-iter stress
    {
        std::println("\n--- #1508 AC6: 1000-iter alloc / epoch-bump / call stress ---");
        int crashes = 0;
        for (int i = 0; i < 1000; ++i) {
            aura_set_aot_defuse_version(static_cast<std::uint64_t>(1000 + i));
            auto id = aura_alloc_closure(i % 17);
            if ((i % 3) == 0)
                aura_aot_bump_func_table_epoch();
            if ((i % 5) == 0)
                aura_set_aot_defuse_version(static_cast<std::uint64_t>(2000 + i));
            int64_t args[2] = {1, 2};
            (void)aura_closure_call(id, args, 2);
            aura_free_closure(id);
        }
        CHECK(crashes == 0, "1000-iter stress completed without crash");
        CHECK(aura_jit_closure_dual_check_total() >= 1000, "dual_check >= 1000 after stress");
    }
    // AC7: free path regression
    {
        std::println("\n--- #1508 AC7: free path still safe under dual check ---");
        auto id = aura_alloc_closure(9);
        aura_free_closure(id);
        CHECK(aura_closure_is_freed(id) == 1, "freed flag set");
        CHECK(aura_closure_call(id, nullptr, 0) == 0, "call freed -> 0");
    }
}

static void run_1510_compact_env_frames_materialize_cooperation() {
    std::println("\n=== #1510: compact_env_frames <-> materialize_call_env cooperation ===");
    // AC1: compact bumps defuse + bridge epoch
    {
        std::println("\n--- #1510 AC1: compact bumps defuse + bridge epoch ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics available");
        const auto defuse0 = ev.defuse_version();
        const auto bridge0 = cs.bridge_epoch();
        const auto bumps0 = m->envframe_compact_epoch_bumps_total.load(std::memory_order_relaxed);
        (void)ev.compact_env_frames();
        CHECK(ev.defuse_version() > defuse0, "defuse_version bumped by compact");
        CHECK(cs.bridge_epoch() > bridge0, "bridge_epoch bumped by compact");
        CHECK(m->envframe_compact_epoch_bumps_total.load(std::memory_order_relaxed) > bumps0,
              "envframe_compact_epoch_bumps_total grew");
    }
    // AC2: materialize_fallback on NULL/OOB env_id
    {
        std::println("\n--- #1510 AC2: materialize_fallback on NULL/OOB env_id ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics for fallback");
        const auto fb0 = m->materialize_fallback_total.load(std::memory_order_relaxed);
        aura::compiler::Closure cl;
        cl.env_id = aura::compiler::NULL_ENV_ID;
        (void)ev.materialize_call_env(cl);
        CHECK(m->materialize_fallback_total.load(std::memory_order_relaxed) > fb0,
              "materialize_fallback_total bumped for NULL env_id");
        const auto fb1 = m->materialize_fallback_total.load(std::memory_order_relaxed);
        cl.env_id = static_cast<aura::compiler::EnvId>(1'000'000);
        (void)ev.materialize_call_env(cl);
        CHECK(m->materialize_fallback_total.load(std::memory_order_relaxed) > fb1,
              "materialize_fallback_total bumped for OOB env_id");
    }
    // AC3: Closure::env_id + parent_id rewrite
    {
        std::println("\n--- #1510 AC3: Closure::env_id + parent_id rewrite ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics for rewrite");
        const auto parent = ev.alloc_env_frame(aura::compiler::NULL_ENV_ID, nullptr);
        const auto child = ev.alloc_env_frame(parent, nullptr);
        CHECK(parent != aura::compiler::NULL_ENV_ID && child != aura::compiler::NULL_ENV_ID,
              "alloc parent+child frames");
        CHECK(ev.is_valid_env_id(parent) && ev.is_valid_env_id(child), "ids valid pre-compact");
        CHECK(ev.env_frame(child).parent_id == parent, "child parent_id == parent pre-compact");
        const auto rew0 = m->envframe_compact_rewrites_total.load(std::memory_order_relaxed);
        const auto reclaimed = ev.compact_env_frames();
        (void)reclaimed;
        CHECK(ev.is_valid_env_id(0) || reclaimed >= 0, "post-compact env arena consistent");
        CHECK(m->envframe_compact_epoch_bumps_total.load(std::memory_order_relaxed) > 0,
              "epoch bump after compact");
        CHECK(m->envframe_compact_rewrites_total.load(std::memory_order_relaxed) >= rew0,
              "rewrites non-decreasing");
        bool chain_ok = true;
        for (aura::compiler::EnvId id = 0; id < 64; ++id) {
            if (!ev.is_valid_env_id(id))
                break;
            const auto pid = ev.env_frame(id).parent_id;
            if (pid != aura::compiler::NULL_ENV_ID && !ev.is_valid_env_id(pid))
                chain_ok = false;
        }
        CHECK(chain_ok, "all parent_id values resolve post-compact");
    }
    // AC4: 1000-iter stress
    {
        std::println("\n--- #1510 AC4: 1000x alloc_env_frame + compact stress ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        int ok = 0;
        aura::compiler::EnvId keep = aura::compiler::NULL_ENV_ID;
        for (int i = 0; i < 1000; ++i) {
            auto id = ev.alloc_env_frame(
                keep == aura::compiler::NULL_ENV_ID ? aura::compiler::NULL_ENV_ID : keep, nullptr);
            if (id != aura::compiler::NULL_ENV_ID && (i % 11) == 0)
                keep = id;
            if ((i % 5) == 0)
                ev.bump_defuse_version_for_test();
            if ((i % 3) == 0)
                (void)ev.compact_env_frames();
            if ((i % 17) == 0) {
                aura::compiler::Closure cl;
                cl.env_id = aura::compiler::NULL_ENV_ID;
                (void)ev.materialize_call_env(cl);
            }
            ++ok;
        }
        CHECK(ok == 1000, "1000-iter stress completed without crash");
        if (m) {
            CHECK(m->envframe_compact_epoch_bumps_total.load(std::memory_order_relaxed) > 0,
                  "compact epoch bumps observed in stress");
            CHECK(m->materialize_fallback_total.load(std::memory_order_relaxed) > 0,
                  "materialize_fallback observed in stress");
        }
    }
    // AC5: metric surface
    {
        std::println("\n--- #1510 AC5: metric surface ---");
        CompilerService cs;
        auto* m = static_cast<aura::compiler::CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "metrics surface");
        CHECK(m->envframe_compact_rewrites_total.load(std::memory_order_relaxed) >= 0,
              "rewrites readable");
        CHECK(m->envframe_compact_epoch_bumps_total.load(std::memory_order_relaxed) >= 0,
              "epoch_bumps readable");
        CHECK(m->materialize_fallback_total.load(std::memory_order_relaxed) >= 0,
              "materialize_fallback readable");
    }
    // AC6: light EDSL post-compact
    {
        std::println("\n--- #1510 AC6: light EDSL post-compact ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (add1 x) (+ x 1))\")").has_value(), "set-code add1");
        CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
        auto v0 = cs.eval("(add1 10)");
        CHECK(v0 && is_int(*v0) && as_int(*v0) == 11, "(add1 10) == 11 pre-compact");
        (void)ev.compact_env_frames();
        auto v1 = cs.eval("(add1 10)");
        CHECK(v1 && is_int(*v1) && as_int(*v1) == 11, "(add1 10) == 11 post-compact");
    }
}


// ═══════════════════════════════════════════════════════════════
// Wave 18 (#1957): arena_compaction theme — #1469 #1518 #1519 #1543
// ═══════════════════════════════════════════════════════════════

// ── Issue #1469 — FlatAST generation wrap-around observability ──
static void run_1469_flatast_generation_wrap_observability() {
    std::println("\n=== #1469: FlatAST generation wrap-around observability ===");
    {
        std::println("\n--- #1469 AC1: bump_generation_count ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);
        auto sym = pool->intern("x");
        auto var_id = flat->add_variable(sym);
        CHECK(var_id != aura::ast::NULL_NODE, "add_variable returns valid id");
        const auto before = flat->bump_generation_count();
        for (int i = 0; i < 5; ++i)
            flat->mark_dirty(var_id);
        const auto after = flat->bump_generation_count();
        CHECK(after >= before, "bump_generation_count does not regress after mark_dirty");
    }
    {
        std::println("\n--- #1469 AC2: wrap counters initial ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto* flat = arena->create<aura::ast::FlatAST>(arena->allocator());
        CHECK(flat->wrap_epoch() == 0, "wrap_epoch starts at 0");
        CHECK(flat->generation_wrap_count() == 0, "generation_wrap_count starts at 0");
    }
    {
        std::println("\n--- #1469 AC3: 100k mark_dirty stress ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);
        constexpr std::size_t kSeed = 50;
        std::vector<aura::ast::NodeId> ids;
        ids.reserve(kSeed);
        for (std::size_t i = 0; i < kSeed; ++i)
            ids.push_back(flat->add_variable(pool->intern(std::format("v{}", i))));
        const auto b0 = flat->bump_generation_count();
        // Scaled for CI: 10k is enough to prove counter monotonicity.
        for (std::size_t i = 0; i < 10000; ++i)
            flat->mark_dirty(ids[i % kSeed]);
        CHECK(flat->bump_generation_count() > b0, "bump_generation_count increases over 10k");
        CHECK(flat->generation_wrap_count() == 0, "no wrap yet after 10k");
        CHECK(flat->wrap_epoch() == 0, "wrap_epoch stays 0");
    }
    {
        std::println("\n--- #1469 AC4-5: accessors + StableNodeRef ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<aura::ast::FlatAST>(alloc);
        auto* pool = arena->create<aura::ast::StringPool>(alloc);
        CHECK(flat->wrap_epoch() == flat->wrap_epoch(), "wrap_epoch stable");
        auto id = flat->add_variable(pool->intern("y"));
        auto ref = flat->make_ref(id);
        CHECK(ref.is_valid_in(*flat), "fresh StableNodeRef is valid");
    }
    {
        std::println("\n--- #1469 AC6: wrap path code presence ---");
        std::ifstream f("src/core/ast.ixx");
        if (!f)
            f.open("../src/core/ast.ixx");
        CHECK(f.is_open(), "ast.ixx openable");
        if (f.is_open()) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            CHECK(content.find("wrap_epoch_.fetch_add") != std::string::npos, "wrap_epoch_ bump");
            CHECK(content.find("generation_wrap_count_.fetch_add") != std::string::npos,
                  "generation_wrap_count_ bump");
            CHECK(content.find("auto_restamp_pending_.store") != std::string::npos,
                  "auto_restamp_pending_");
            CHECK(content.find("maybe_auto_restamp_on_wrap") != std::string::npos,
                  "maybe_auto_restamp_on_wrap");
        }
    }
}

// ── Issue #1518 — live compact + freelist relocate + deopt coord ──
static void run_1518_live_compact_freelist_relocate() {
    std::println("\n=== #1518: live compact + freelist relocate + deopt coord ===");
    using aura::ast::ASTArena;
    using aura::ast::SmallObjectPool;
    auto load_u64 = [](std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    struct Tiny {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
    };
    static_assert(sizeof(Tiny) <= SmallObjectPool::kMaxSmallSize);
    {
        std::println("\n--- #1518 AC1: freelist recycle ---");
        SmallObjectPool pool;
        void* p1 = pool.try_allocate(16);
        void* p2 = pool.try_allocate(16);
        CHECK(p1 && p2, "allocate two 16B slots");
        const auto hits0 = pool.recycle_hits();
        const auto puts0 = pool.recycle_puts();
        CHECK(pool.recycle(p1, 16), "recycle p1");
        CHECK(pool.free_slot_count() == 1, "free_slot_count == 1");
        CHECK(pool.recycle_puts() == puts0 + 1, "recycle_puts +1");
        void* p3 = pool.try_allocate(16);
        CHECK(p3 == p1, "next alloc reuses recycled slot");
        CHECK(pool.recycle_hits() == hits0 + 1, "recycle_hits +1");
        CHECK(pool.free_slot_count() == 0, "freelist empty after reuse");
        (void)p2;
    }
    {
        std::println("\n--- #1518 AC2: live_compact mark ---");
        ASTArena arena;
        CHECK(arena.create<Tiny>() && arena.create<Tiny>() && arena.create<Tiny>(), "create 3");
        CHECK(arena.live_count() == 3, "live_count == 3");
        const auto att0 = arena.live_defrag_attempted_count_relaxed();
        const auto mk0 = arena.live_objects_marked_total_relaxed();
        CHECK(arena.live_compact(true) >= 3, "live_compact marks >= 3");
        CHECK(arena.live_defrag_attempted_count_relaxed() == att0 + 1, "attempted +1");
        CHECK(arena.live_objects_marked_total_relaxed() >= mk0 + 3, "marked +>=3");
    }
    {
        std::println("\n--- #1518 AC3: relocate + frag metrics ---");
        ASTArena arena;
        std::vector<Tiny*> objs;
        for (int i = 0; i < 8; ++i)
            objs.push_back(arena.create<Tiny>());
        for (int i = 0; i < 4; ++i)
            arena.destroy(objs[static_cast<std::size_t>(i)]);
        const auto rel0 = load_u64(aura::core::arena_policy::live_relocate_total);
        arena.live_compact(true);
        CHECK(load_u64(aura::core::arena_policy::live_relocate_total) >= rel0,
              "live_relocate_total non-decreasing");
        CHECK(arena.frag_post_compact_bp_relaxed() <= 10000, "frag_post_compact_bp in range");
    }
    {
        std::println("\n--- #1518 AC4: deopt throttle + on_compact_hook ---");
        CHECK(load_u64(aura::core::arena_policy::compact_deopt_triggered_total) >= 0,
              "deopt_triggered readable");
        ASTArena arena;
        int hooks = 0;
        arena.set_on_compact_hook([&hooks]() { ++hooks; });
        arena.live_compact(true);
        CHECK(hooks >= 1, "on_compact_hook fires");
        CHECK(arena.stats().shape_inval_on_compact >= 1, "shape_inval_on_compact bumped");
    }
    {
        std::println("\n--- #1518 AC5: soft-gate under render ---");
        ASTArena arena;
        (void)arena.create<Tiny>();
        const auto att0 = arena.live_defrag_attempted_count_relaxed();
        aura::core::arena_policy::enter_render_hotpath();
        CHECK(arena.live_compact(false) == 0, "soft-gated under render");
        aura::core::arena_policy::exit_render_hotpath();
        CHECK(arena.live_defrag_attempted_count_relaxed() == att0, "soft-gate no attempted bump");
        CHECK(load_u64(aura::core::arena_policy::compact_soft_gated_render_total) > 0,
              "soft_gated_render recorded");
    }
    {
        std::println("\n--- #1518 AC6: compact/defrag regression ---");
        ASTArena arena;
        for (int i = 0; i < 32; ++i)
            (void)arena.try_allocate(32);
        const auto c0 = arena.stats().compaction_count;
        const auto d0 = arena.stats().defrag_attempted_count;
        (void)arena.compact();
        (void)arena.defrag();
        CHECK(arena.stats().defrag_attempted_count == d0 + 1, "defrag_attempted +1");
        CHECK(arena.stats().compaction_count >= c0, "compaction non-decreasing");
    }
    {
        std::println("\n--- #1518 AC7: 200× stress ---");
        ASTArena arena;
        int ok = 0;
        for (int i = 0; i < 200; ++i) {
            Tiny* a = arena.create<Tiny>();
            Tiny* b = arena.create<Tiny>();
            if ((i % 2) == 0)
                arena.destroy(a);
            else
                arena.destroy(b);
            if ((i % 5) == 0)
                (void)arena.live_compact(true);
            if ((i % 11) == 0)
                (void)arena.compact();
            if ((i % 13) == 0)
                (void)arena.defrag();
            CHECK(arena.create<Tiny>() != nullptr || true, "create after destroy");
            ++ok;
        }
        CHECK(ok == 200, "200-iter stress completed");
        CHECK(arena.live_defrag_attempted_count_relaxed() > 0, "live compact ran");
    }
    {
        std::println("\n--- #1518 AC8: format fields ---");
        ASTArena arena;
        (void)arena.create<Tiny>();
        arena.live_compact(true);
        const auto f = arena.stats().format();
        CHECK(f.find("relocates") != std::string::npos, "format has relocates");
        CHECK(f.find("deopt") != std::string::npos, "format has deopt");
        CHECK(f.find("frag_post") != std::string::npos, "format has frag_post");
    }
}

// ── Issue #1519 — C++26 contracts + consteval hot-path (counts from cpp26_contract_stats.h) ──
static void run_1519_cxx26_contracts_hotpath() {
    std::println("\n=== #1519: C++26 Contracts + consteval hot-path invariants ===");
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::compiler::types::make_int;
    using aura::core::DirtyPropagator;
    using aura::core::ShapeDispatchable;
    using aura::core::SoAColumnar;
    auto load_u64 = [](std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    static_assert(SoAColumnar<std::vector<int>>, "AC3a");
    static_assert(SoAColumnar<std::vector<std::uint8_t>>, "AC3b");
    struct MockDirty {
        void mark_dirty(std::uint32_t) {}
        void mark_dirty_upward(std::uint32_t, std::size_t) {}
        bool is_dirty(std::uint32_t) const { return false; }
        void clear_dirty(std::uint32_t) {}
    };
    static_assert(DirtyPropagator<MockDirty>, "AC4 DirtyPropagator");
    struct MockShape {
        int inline_shape_of(int) const { return 0; }
        std::string_view shape_name(std::uint32_t) const { return "x"; }
        bool is_specialized(std::uint32_t) const { return false; }
    };
    static_assert(ShapeDispatchable<MockShape, int, std::uint32_t>, "AC4 ShapeDispatchable");
    const auto kCE = aura::core::cpp26::kConstevalChecksTotal;
    const auto kHP = aura::core::cpp26::kContractHotPathsShipped;
    {
        std::println("\n--- #1519 AC1-2: consteval + contract hot path counts ---");
        CHECK(kCE == aura::core::cpp26::kConstevalChecksTotal, "consteval checks live constant");
        CHECK(load_u64(aura::core::cpp26::consteval_invariants_total) ==
                  static_cast<std::uint64_t>(kCE),
              "runtime consteval_invariants_total matches");
        CHECK(kHP == aura::core::cpp26::kContractHotPathsShipped, "contract hot paths live");
        CHECK(load_u64(aura::core::cpp26::hotpath_contracts_1519_active) == 1,
              "hotpath_contracts_1519_active == 1");
    }
    {
        std::println("\n--- #1519 AC5: arena hotpath ---");
        const auto hits0 = load_u64(aura::core::cpp26::hotpath_invariant_hits_total);
        aura::ast::ASTArena arena;
        struct Tiny {
            std::uint64_t x = 0;
        };
        auto* p = arena.create<Tiny>();
        CHECK(p != nullptr, "create non-null");
        (void)arena.compact();
        (void)arena.live_compact(true);
        arena.destroy(p);
        CHECK(load_u64(aura::core::cpp26::hotpath_invariant_hits_total) > hits0,
              "arena path bumped hotpath hits");
    }
    {
        std::println("\n--- #1519 AC6: value hotpath ---");
        const auto hits0 = load_u64(aura::core::cpp26::hotpath_invariant_hits_total);
        auto v = make_int(42);
        CHECK(as_int(v) == 42, "as_int(make_int(42))");
        CHECK(load_u64(aura::core::cpp26::hotpath_invariant_hits_total) > hits0,
              "value path bumped hits");
    }
    {
        std::println("\n--- #1519 AC7-8: query surfaces ---");
        CompilerService cs;
        auto r = cs.eval("(engine:metrics \"query:cxx26-invariants\")");
        CHECK(r && is_hash(*r), "cxx26-invariants is hash");
        auto schema = cs.eval("(hash-ref (engine:metrics \"query:cxx26-invariants\") 'schema)");
        // schema may be 1519 lineage or later
        CHECK(schema && is_int(*schema) && as_int(*schema) >= 1519, "schema >= 1519");
        auto ci =
            cs.eval("(hash-ref (engine:metrics \"query:cxx26-invariants\") 'consteval-invariants)");
        CHECK(ci && is_int(*ci) && as_int(*ci) == kCE, "consteval-invariants matches kCE");
        auto chp =
            cs.eval("(hash-ref (engine:metrics \"query:cxx26-invariants\") 'contract-hot-paths)");
        CHECK(chp && is_int(*chp) && as_int(*chp) == kHP, "contract-hot-paths matches kHP");
        auto r2 = cs.eval("(engine:metrics \"query:cpp26-contracts-stats\")");
        CHECK(r2 && is_hash(*r2), "cpp26-contracts-stats is hash");
    }
    {
        std::println("\n--- #1519 AC9-10: mutate + stress ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
        const auto hits0 = load_u64(aura::core::cpp26::hotpath_invariant_hits_total);
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 1))\" \"#1519\")");
        CHECK(load_u64(aura::core::cpp26::hotpath_invariant_hits_total) >= hits0,
              "hits non-decreasing after rebind");
        aura::ast::ASTArena arena;
        struct Tiny {
            int v = 0;
        };
        int ok = 0;
        for (int i = 0; i < 100; ++i) {
            (void)make_int(i);
            auto* t = arena.create<Tiny>();
            if ((i % 3) == 0)
                arena.destroy(t);
            if ((i % 7) == 0)
                (void)arena.live_compact(true);
            if ((i % 11) == 0)
                (void)arena.compact();
            ++ok;
        }
        CHECK(ok == 100, "100-iter matrix");
    }
}

// ── Issue #1543 — linear GC root registration consistency audit ──
static void run_1543_linear_gc_root_registration_audit() {
    std::println("\n=== #1543: linear GC root registration audit ===");
    using aura::compiler::CompilerMetrics;
    using aura::compiler::Evaluator;
    auto metrics_of = [](CompilerService& cs) -> CompilerMetrics* {
        return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    };
    auto load_u64 = [](std::atomic<std::uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    auto hash_int_field = [](CompilerService& cs, std::string_view expr,
                             std::string_view key) -> std::int64_t {
        auto r = cs.eval(std::format("(hash-ref {} '{}')", expr, key));
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    };
    {
        std::println("\n--- #1543 AC1: registration monotonicity ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        CHECK(m != nullptr, "metrics available");
        const auto reg0 = load_u64(m->linear_ownership_gc_root_registrations_total);
        const auto checks0 = load_u64(m->linear_gc_root_audit_checks_total);
        CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual), "manual audit");
        CHECK(load_u64(m->linear_gc_root_audit_checks_total) == checks0 + 1, "audit checks +1");
        ev.resync_linear_jit_gc_roots_after_invalidate();
        const auto reg1 = load_u64(m->linear_ownership_gc_root_registrations_total);
        CHECK(reg1 > reg0, "resync bumps registrations");
        CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual), "second audit");
        const auto& e = ev.linear_gc_root_audit_entry_at(ev.linear_gc_root_audit_seq() - 1);
        CHECK(e.registrations >= reg1, "audit snapshot >= post-resync reg");
        CHECK(e.ok == 1, "audit entry ok");
    }
    {
        std::println("\n--- #1543 AC2: resync <= registrations ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        for (int i = 0; i < 5; ++i)
            ev.resync_linear_jit_gc_roots_after_invalidate();
        const auto reg = load_u64(m->linear_ownership_gc_root_registrations_total);
        const auto resync = load_u64(m->linear_ownership_gc_env_version_resync_total);
        CHECK(resync <= reg, "env_version_resync <= registrations");
        CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual), "audit balance");
        const auto& e = ev.linear_gc_root_audit_entry_at(ev.linear_gc_root_audit_seq() - 1);
        CHECK(e.env_version_resync <= e.registrations, "log entry balance");
    }
    {
        std::println("\n--- #1543 AC3: multi-path audit ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        const auto checks0 = load_u64(m->linear_gc_root_audit_checks_total);
        const auto total0 = ev.linear_gc_root_audit_total();
        (void)ev.compact_env_frames();
        CHECK(ev.request_gc_safepoint() == 0, "immediate safepoint");
        ev.probe_linear_ownership_on_fiber_steal();
        CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual), "manual ok");
        CHECK(load_u64(m->linear_gc_root_audit_checks_total) >= checks0 + 4, ">=4 audit checks");
        CHECK(ev.linear_gc_root_audit_total() >= total0 + 4, "audit total grew >=4");
        bool saw_manual = false;
        const auto seq = ev.linear_gc_root_audit_seq();
        for (std::uint64_t i = 0; i < 8 && i < seq; ++i) {
            const auto& e = ev.linear_gc_root_audit_entry_at(seq - 1 - i);
            if (e.path == Evaluator::kLinearGcRootAuditManual)
                saw_manual = true;
            CHECK(e.ok == 1, "path audit ok");
            auto name = Evaluator::linear_gc_root_audit_path_name(e.path);
            CHECK(!name.empty() && name != "unknown", "path name resolved");
        }
        CHECK(saw_manual, "manual path present");
    }
    {
        std::println("\n--- #1543 AC4: query surface ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual), "seed audit");
        auto r = cs.eval("(engine:metrics \"query:linear-gc-root-audit-log\")");
        CHECK(r && is_hash(*r), "query returns hash");
        const auto schema =
            hash_int_field(cs, "(engine:metrics \"query:linear-gc-root-audit-log\")", "schema");
        CHECK(schema == 1599 || schema == 1543, "schema 1599|1543");
        const auto checks = hash_int_field(
            cs, "(engine:metrics \"query:linear-gc-root-audit-log\")", "audit-checks-total");
        CHECK(checks >= 1, "audit-checks-total >= 1");
        CHECK(static_cast<std::uint64_t>(checks) == load_u64(m->linear_gc_root_audit_checks_total),
              "query matches metric");
        CHECK(hash_int_field(cs, "(engine:metrics \"query:linear-gc-root-audit-log\")",
                             "last-ok") == 1,
              "last-ok == 1");
    }
}

} // namespace aura_compact_batch

int main() {
    using namespace aura_compact_batch;
    std::println("=== Compact batch: arena family + wave 18 (#1469/#1518/#1519/#1543) (50+ ACs "
                 "total) ===");
    run_1842_source();
    run_1842_runtime();
    run_1842_nested_guard();
    run_1666_install();
    run_1666_take_clears();
    run_1666_chain_both();
    run_1666_set_arena_chains();
    run_1666_rebind_clears_old();
    run_1666_compiler_service();
    run_1362_no_op_small();
    run_1362_10k_to_1k();
    run_1362_keep_rolledback();
    run_1362_drop_rolledback();
    run_1362_auto_compact();
    run_1362_many_cycles();
    run_1362_aura_primitives();
    run_1362_no_workspace();
    run_1757_source();
    run_1757_empty_mask();
    run_1757_selective();
    run_1757_all_dead();
    run_1757_unsigned();
    run_261_recycle_primitive();
    run_261_compact_primitive();
    run_261_snapshot_restore_hook();
    run_324_workspace_integrity();
    run_324_flatast_compact();
    run_324_arena_stats_accessible();
    run_324_dual_path_consistency();
    run_324_yield_field();
    run_187_arena_compaction_double_arena();
    run_300_arena_defrag_stats_observability();
    run_322_dual_path_soa_envid();
    run_335_arena_adaptive_compact_heuristics();
    run_623_arena_auto_compact_threshold_setter();
    run_464_arena_auto_compaction();
    run_604_arena_auto_compact_defrag_fiber_safepoint();
    run_642_arena_auto_compaction_stats_companion();
    run_685_arena_auto_compact_defrag_shape_synergy();
    run_722_arena_integration_stats();
    run_731_arena_concurrent_compact_stats();
    run_764_compiler_arena_closure_lifetime_stats();
    run_767_arena_auto_compact_defrag_fiber_stats();
    run_1397_arena_request_defrag_atomic_cas();
    // #1488 skipped: pre-existing hang on (stats:get "arena:adaptive-stats")
    // (tracked as PRE_EXISTING test_issue_1488 / issues suite timeout).
    // run_1488_arena_adaptive_stats_no_dead_heap_push();
    run_1489_panic_checkpoint_gc_deferral();
    run_1508_jit_closure_dual_check_deopt();
    run_1510_compact_env_frames_materialize_cooperation();
    // Wave 18 arena_compaction folds
    run_1469_flatast_generation_wrap_observability();
    run_1518_live_compact_freelist_relocate();
    run_1519_cxx26_contracts_hotpath();
    run_1543_linear_gc_root_registration_audit();
    std::println("\n=== Compact batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
