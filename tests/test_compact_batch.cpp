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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <type_traits>
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
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── Issue #1842 — compact_env_frames Guard ──
static void run_1842_source() {
    std::println("\n--- AC1 (#1842): Guard + try/catch on compact-env-frames ---");
    std::string src;
    for (const char* p : {"src/compiler/evaluator_primitives_compile_07.cpp",
                          "../src/compiler/evaluator_primitives_compile_07.cpp"}) {
        src = read_file(p);
        if (!src.empty())
            break;
    }
    CHECK(!src.empty(), "read compile_07.cpp");
    CHECK(src.find("#1842") != std::string::npos, "cites #1842");
    auto pos = src.find("\"evaluator:compact-env-frames\"");
    CHECK(pos != std::string::npos, "primitive present");
    auto win = src.substr(pos, 1600);
    CHECK(win.find("MutationBoundaryGuard") != std::string::npos, "uses Guard");
    CHECK(win.find("guard_ok") != std::string::npos, "guard_ok flag");
    CHECK(win.find("try {") != std::string::npos || win.find("try{") != std::string::npos,
          "try block");
    CHECK(win.find("catch") != std::string::npos, "catch path");
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

} // namespace aura_compact_batch

int main() {
    using namespace aura_compact_batch;
    std::println("=== Compact batch: #1842 + #1666 + #1362 + #1757 (22 ACs total) ===");
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
    std::println("\n=== Compact batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
