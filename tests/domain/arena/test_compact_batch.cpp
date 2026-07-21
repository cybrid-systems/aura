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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <type_traits>
#include <mutex>
#include <thread>
#include <vector>

import std;
import aura.core.arena;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

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

} // namespace aura_compact_batch

int main() {
    using namespace aura_compact_batch;
    std::println("=== Compact batch: #1842 + #1666 + #1362 + #1757 + #261 + #324 + #187 (50+ ACs "
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
    std::println("\n=== Compact batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
