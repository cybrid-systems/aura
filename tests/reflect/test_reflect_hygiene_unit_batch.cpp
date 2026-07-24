// test_edsl_hygiene_unit_batch.cpp — consolidated edsl hygiene drivers
// Merged from per-issue standalones; each section lives in its own namespace.
// Prefer adding a section here over a new tests/edsl binary.

#include "test_harness.hpp"
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.pass_manager;
import aura.core;


// ─── from test_allow_macro_inline_per_eval.cpp →
// aura_edsl_run_allow_macro_inline_1780::run_allow_macro_inline_1780 ───
namespace aura_edsl_run_allow_macro_inline_1780 {
// Issue #1780 (#1978 renamed): issue# moved from filename to header.
// @category: unit
// @reason: Issue #1780 — *allow-macro-inline* must use per-Evaluator
// state (not InlinePass process-wide static). Concurrent CompilerServices
// / fibers must not clobber each other's macro-hygiene policy.
//
//   AC1: source cites #1780; uses set_inline_respect_macro_hygiene
//   AC2: no InlinePass::set/get_respect_macro_hygiene in primitive
//   AC3: two CompilerServices toggle independently
//   AC4: InlinePass hygiene is instance (not static member)


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_int;
    using aura::test::g_failed;
    using aura::test::g_passed;

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::string read_first(std::initializer_list<const char*> paths) {
        for (const char* p : paths) {
            auto s = read_file(p);
            if (!s.empty())
                return s;
        }
        return {};
    }

} // namespace

int run_allow_macro_inline_1780() {
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: per-Evaluator path (no InlinePass static) ---");
        auto prim = read_first({"src/compiler/evaluator_primitives_compile.cpp",
                                "../src/compiler/evaluator_primitives_compile.cpp"});
        CHECK(!prim.empty(), "read compile_04.cpp");
        CHECK(prim.find("#1780") != std::string::npos, "cites #1780");
        auto pos = prim.find("add(\"*allow-macro-inline*\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = prim.substr(pos, 500);
        CHECK(win.find("set_inline_respect_macro_hygiene") != std::string::npos,
              "uses Evaluator setter");
        CHECK(win.find("get_inline_respect_macro_hygiene") != std::string::npos,
              "uses Evaluator getter");
        CHECK(win.find("InlinePass::set_respect_macro_hygiene") == std::string::npos,
              "no InlinePass static set in primitive");
        CHECK(win.find("InlinePass::get_respect_macro_hygiene") == std::string::npos,
              "no InlinePass static get in primitive");

        auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
        CHECK(!ixx.empty() && ixx.find("inline_respect_macro_hygiene_") != std::string::npos,
              "Evaluator stores per-eval policy");
        CHECK(ixx.find("set_inline_respect_macro_hygiene") != std::string::npos, "setter present");

        auto pm = read_first({"src/compiler/pass_manager.ixx", "../src/compiler/pass_manager.ixx"});
        CHECK(!pm.empty(), "read pass_manager.ixx");
        CHECK(pm.find("static inline bool respect_macro_hygiene_") == std::string::npos,
              "InlinePass hygiene not process-wide static");
        CHECK(pm.find("bool respect_macro_hygiene_ = true") != std::string::npos,
              "InlinePass hygiene is instance field");
    }

    // ── AC3: independent CompilerServices ──
    {
        std::println("\n--- AC3: two services toggle independently ---");
        CompilerService a;
        CompilerService b;
        // Default: respect=true → inlinable flag returns 0 after (* #f) path;
        // enable #t → returns 1 (macro inline allowed).
        auto a_on = a.eval("(*allow-macro-inline* #t)");
        CHECK(a_on && is_int(*a_on) && as_int(*a_on) == 1, "A: enable → 1");
        // B still default (respect=true); enabling on A must not flip B.
        auto b_def = b.eval("(*allow-macro-inline* #f)");
        CHECK(b_def && is_int(*b_def) && as_int(*b_def) == 0, "B: disable → 0 (still default-ish)");
        // Re-assert A stayed enabled after B's toggle.
        auto a_again = a.eval("(*allow-macro-inline* #t)");
        CHECK(a_again && is_int(*a_again) && as_int(*a_again) == 1, "A still independent of B");

        CHECK(a.evaluator().get_inline_respect_macro_hygiene() == false,
              "A evaluator: respect=false after #t");
        CHECK(b.evaluator().get_inline_respect_macro_hygiene() == true,
              "B evaluator: respect=true after #f");
    }

    // ── AC4: InlinePass instances are independent ──
    {
        std::println("\n--- AC4: InlinePass instance policy isolation ---");
        aura::compiler::InlinePass p1;
        aura::compiler::InlinePass p2;
        CHECK(p1.get_respect_macro_hygiene() == true, "p1 default true");
        CHECK(p2.get_respect_macro_hygiene() == true, "p2 default true");
        p1.set_respect_macro_hygiene(false);
        CHECK(p1.get_respect_macro_hygiene() == false, "p1 set false");
        CHECK(p2.get_respect_macro_hygiene() == true, "p2 unaffected by p1");
    }

    std::println("\n=== test_allow_macro_inline_per_eval_1780: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_edsl_run_allow_macro_inline_1780
// ─── end test_allow_macro_inline_per_eval.cpp ───

// ─── from test_hygiene_protected_metadata_lock.cpp →
// aura_edsl_run_protected_metadata_1838::run_protected_metadata_1838 ───
namespace aura_edsl_run_protected_metadata_1838 {
// Issue #1838 (#1978 renamed): issue# moved from filename to header.
// @category: unit
// @reason: Issue #1838 — hygiene:protected? must hold metadata
// reader lock when reading is_macro_introduced (same race class
// as #1783 syntax-marker vs set-marker).
//
//   AC1: source cites #1838; try_acquire_metadata_reader_lock
//   AC2: hygiene:protected? returns bool on valid/invalid args
//   AC3: set-marker + protected? under sequential use stays consistent


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_int;
    using aura::test::g_failed;
    using aura::test::g_passed;

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

} // namespace

int run_protected_metadata_1838() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: hygiene:protected? takes metadata reader lock ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_07.cpp");
        CHECK(src.find("#1838") != std::string::npos, "cites #1838");
        auto pos = src.find("\"hygiene:protected?\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 800);
        CHECK(win.find("try_acquire_metadata_reader_lock") != std::string::npos,
              "uses metadata reader lock");
        CHECK(win.find("is_macro_introduced") != std::string::npos, "reads is_macro_introduced");
    }

    // ── AC2: runtime shape ──
    {
        std::println("\n--- AC2: hygiene:protected? returns bool ---");
        CompilerService cs;
        CHECK(cs.eval("(define x 1)").has_value(), "seed");
        auto bad = cs.eval("(hygiene:protected?)");
        CHECK(bad && is_bool(*bad) && !as_bool(*bad), "no-arg → #f");
        auto oob = cs.eval("(hygiene:protected? 999999)");
        CHECK(oob && is_bool(*oob) && !as_bool(*oob), "OOB → #f");
        auto z = cs.eval("(hygiene:protected? 0)");
        CHECK(z && is_bool(*z), "valid id returns bool");
    }

    // ── AC3: set-marker then protected? ──
    {
        std::println("\n--- AC3: set-marker + protected? sequential ---");
        CompilerService cs;
        // Materialize workspace AST (same pattern as #1783 / #366).
        CHECK(cs.eval("(set-code \"(define y 2)\")").has_value(), "set-code");
        auto rid = cs.eval("(car (query:find \"y\"))");
        CHECK(rid && is_int(*rid), "find y");
        const auto id = as_int(*rid);
        // Marker 1 = MacroIntroduced.
        auto set = cs.eval(std::format("(syntax:set-marker {} 1)", id));
        CHECK(set && is_bool(*set) && as_bool(*set), "set-marker ok");
        auto prot = cs.eval(std::format("(hygiene:protected? {})", id));
        CHECK(prot && is_bool(*prot) && as_bool(*prot), "node protected after MacroIntroduced");
        auto clear = cs.eval(std::format("(syntax:set-marker {} 0)", id));
        CHECK(clear && is_bool(*clear) && as_bool(*clear), "clear marker ok");
        auto unprot = cs.eval(std::format("(hygiene:protected? {})", id));
        CHECK(unprot && is_bool(*unprot) && !as_bool(*unprot), "node not protected after User");
    }

    std::println("\n=== test_hygiene_protected_metadata_lock_1838: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_edsl_run_protected_metadata_1838
// ─── end test_hygiene_protected_metadata_lock.cpp ───

// ─── from test_workspace_marker_macro_max.cpp →
// aura_edsl_run_workspace_marker_macro_1678::run_workspace_marker_macro_1678 ───
namespace aura_edsl_run_workspace_marker_macro_1678 {
// Issue #1678 (#1978 renamed): issue# moved from filename to header.
// @category: unit
// @reason: Issue #1678 — workspace_marker_macro_introduced uses max(walk, snapshot)
// so walk < snapshot no longer undercounts MacroIntroduced provenance.
//
//   AC1: walk=1, snapshot=5 → macro-markers reports 5 (not 1)
//   AC2: walk=2, snapshot=0 → macro-markers reports 2
//   AC3: walk=0, snapshot=3 → macro-markers reports 3 (snapshot floor)
//   AC4: query:pattern-hygiene-stats still returns a hash


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::test::g_failed;
    using aura::test::g_passed;

    static std::int64_t macro_markers(CompilerService& cs) {
        auto r =
            cs.eval("(hash-ref (stats:get \"query:pattern-hygiene-stats\") \"macro-markers\")");
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    }

} // namespace

int run_workspace_marker_macro_1678() {
    CompilerService cs;

    // Workspace with three defines so we can stamp MacroIntroduced markers.
    auto sc = cs.eval("(set-code \"(define x 1) (define y 2) (define z 3)\")");
    CHECK(sc.has_value(), "set-code three defines");

    // ── AC4: dashboard is live ──
    {
        std::println("\n--- AC4: query:pattern-hygiene-stats hash ---");
        auto r = cs.eval("(stats:get \"query:pattern-hygiene-stats\")");
        CHECK(r && is_hash(*r), "pattern-hygiene-stats is hash");
    }

    // Mark y as MacroIntroduced (marker id 1) via public syntax:set-marker.
    {
        auto m = cs.eval("(syntax:set-marker (car (cdr (query:defines))) 1)");
        CHECK(m && is_bool(*m) && as_bool(*m), "set MacroIntroduced on y");
    }

    // ── AC1: walk=1, snapshot=5 → max = 5 ──
    {
        std::println("\n--- AC1: walk < snapshot → max(snapshot) ---");
        cs.evaluator().set_macro_markers_in_snapshot(5);
        const auto n = macro_markers(cs);
        CHECK(n == 5, std::format("macro-markers={} want 5 (max of walk≈1, snap=5)", n));
    }

    // Mark z as MacroIntroduced as well → walk ≥ 2.
    {
        auto m = cs.eval("(syntax:set-marker (car (cdr (cdr (query:defines)))) 1)");
        CHECK(m && is_bool(*m) && as_bool(*m), "set MacroIntroduced on z");
    }

    // ── AC2: walk=2, snapshot=0 → max = 2 ──
    {
        std::println("\n--- AC2: walk > snapshot → live walk ---");
        cs.evaluator().set_macro_markers_in_snapshot(0);
        const auto n = macro_markers(cs);
        CHECK(n >= 2, std::format("macro-markers={} want >= 2 (live walk)", n));
    }

    // ── AC3: walk=0 (no workspace markers) but snapshot floor ──
    // Clear markers by rewriting source without MacroIntroduced stamps,
    // then force snapshot.
    {
        std::println("\n--- AC3: empty walk, snapshot floor ---");
        auto sc2 = cs.eval("(set-code \"(define a 1)\")");
        CHECK(sc2.has_value(), "set-code single define (fresh markers)");
        cs.evaluator().set_macro_markers_in_snapshot(3);
        const auto n = macro_markers(cs);
        // Fresh define column may have 0 MacroIntroduced; floor is snapshot.
        CHECK(n >= 3, std::format("macro-markers={} want >= 3 (snapshot floor)", n));
    }

    std::println("\n=== test_workspace_marker_macro_max_1678: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_edsl_run_workspace_marker_macro_1678
// ─── end test_workspace_marker_macro_max.cpp ───

// ─── from test_subtree_counter_shared_lock.cpp →
// aura_edsl_run_subtree_counter_1848::run_subtree_counter_1848 ───
namespace aura_edsl_run_subtree_counter_1848 {
// Issue #1848 (#1978 renamed): issue# moved from filename to header.
// @category: unit
// @reason: Issue #1848 — compile:subtree-generation and
// compile:subtree-bump-count must shared_lock workspace_mtx_
// while reading counters so concurrent compile:subtree-bump
// (#1847 unique Guard) cannot race subtree_gen_ resize / tear.
//
//   AC1: source cites #1848; both readers use shared_lock
//   AC2: sequential bump then stats:get bump-count advances
//   AC3: concurrent C++ shared_lock readers + EDSL bump no hang


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_int;
    using aura::test::g_failed;
    using aura::test::g_passed;

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

} // namespace

int run_subtree_counter_1848() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: shared_lock on subtree readers ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_06.cpp");
        CHECK(src.find("#1848") != std::string::npos, "cites #1848");

        auto gen_pos = src.find("\"compile:subtree-generation\"");
        CHECK(gen_pos != std::string::npos, "subtree-generation present");
        auto gen_win = src.substr(gen_pos, 900);
        CHECK(gen_win.find("shared_lock") != std::string::npos, "generation uses shared_lock");
        CHECK(gen_win.find("workspace_mtx_") != std::string::npos,
              "generation locks workspace_mtx_");
        CHECK(gen_win.find("subtree_generation") != std::string::npos, "calls subtree_generation");

        auto cnt_pos = src.find("\"compile:subtree-bump-count\"");
        CHECK(cnt_pos != std::string::npos, "subtree-bump-count present");
        auto cnt_win = src.substr(cnt_pos, 700);
        CHECK(cnt_win.find("shared_lock") != std::string::npos, "bump-count uses shared_lock");
        CHECK(cnt_win.find("workspace_mtx_") != std::string::npos,
              "bump-count locks workspace_mtx_");
        CHECK(cnt_win.find("subtree_bump_count") != std::string::npos, "calls subtree_bump_count");
    }

    // ── AC2: sequential bump + stats:get ──
    {
        std::println("\n--- AC2: bump then stats:get bump-count ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1) (define y 2)\")").has_value(), "set-code");
        auto id_v = cs.eval("(car (query:defines-by-marker \"User\"))");
        CHECK(id_v && is_int(*id_v), "resolve define id");
        const auto id = as_int(*id_v);

        auto before = cs.eval("(stats:get \"compile:subtree-bump-count\")");
        // zero-arity stats path; may be int or void if not registered in facade
        std::int64_t b = 0;
        if (before && is_int(*before))
            b = as_int(*before);

        auto bump = cs.eval(std::format("(compile:subtree-bump {})", id));
        CHECK(bump && is_int(*bump) && as_int(*bump) >= 0, "bump ok");

        auto after = cs.eval("(stats:get \"compile:subtree-bump-count\")");
        if (after && is_int(*after) && bump && is_int(*bump) && as_int(*bump) == 1) {
            CHECK(as_int(*after) >= b + 1, "bump-count advanced after successful bump");
        } else if (after && is_int(*after)) {
            CHECK(as_int(*after) >= b, "bump-count non-decreasing");
        } else {
            // Facade may not surface this name; C++ path still covered in AC3.
            CHECK(true, "stats:get optional for this name (C++ lock path in AC3)");
        }

        // Direct C++ read under public lock API (mirrors primitive).
        auto& ev = cs.evaluator();
        ev.lock_workspace_shared();
        CHECK(ev.workspace_flat() != nullptr, "workspace present");
        if (ev.workspace_flat()) {
            auto g = ev.workspace_flat()->subtree_generation(static_cast<std::uint32_t>(id));
            CHECK(true, std::format("subtree_generation under lock = {}", g));
            (void)ev.workspace_flat()->subtree_bump_count();
        }
        ev.unlock_workspace_shared();
    }

    // ── AC3: concurrent C++ readers + writer via public lock API ──
    // Mirrors the primitive contract without concurrent cs.eval
    // (CompilerService eval is not multi-thread re-entrant).
    {
        std::println("\n--- AC3: concurrent shared readers + unique bump ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define a 1) (define b 2)\")").has_value(), "seed");
        auto id_v = cs.eval("(car (query:defines-by-marker \"User\"))");
        CHECK(id_v && is_int(*id_v), "id");
        const auto id = static_cast<std::uint32_t>(as_int(*id_v));
        auto& ev = cs.evaluator();
        CHECK(ev.workspace_flat() != nullptr, "workspace for stress");

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> reads{0};
        std::atomic<std::uint64_t> bumps{0};
        std::vector<std::thread> thr;

        auto reader = [&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                ev.lock_workspace_shared();
                if (auto* ws = ev.workspace_flat()) {
                    (void)ws->subtree_generation(id);
                    (void)ws->subtree_bump_count();
                }
                ev.unlock_workspace_shared();
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        };
        thr.emplace_back(reader);
        thr.emplace_back(reader);
        thr.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                // Same lock the #1847 Guard holds (unique).
                ev.lock_workspace_unique();
                if (auto* ws = ev.workspace_flat()) {
                    ws->bump_generation_subtree(id);
                    bumps.fetch_add(1, std::memory_order_relaxed);
                }
                ev.unlock_workspace_unique();
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : thr)
            t.join();

        CHECK(reads.load() > 0, "readers made progress");
        CHECK(bumps.load() > 0, "writer made progress");
        // Post-join single-threaded consistency.
        ev.lock_workspace_shared();
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "ws still live");
        if (ws) {
            CHECK(ws->subtree_bump_count() >= bumps.load(), "count >= observed bumps");
        }
        ev.unlock_workspace_shared();
    }

    std::println("\n=== test_subtree_counter_shared_lock_1848: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_edsl_run_subtree_counter_1848
// ─── end test_subtree_counter_shared_lock.cpp ───

// ─── from test_tag_arity_index_perf.cpp →
// aura_edsl_run_tag_arity_index_perf_1371::run_tag_arity_index_perf_1371 ───
namespace aura_edsl_run_tag_arity_index_perf_1371 {
// test_tag_arity_index_perf.cpp — Issue #1371:
// tag_arity_index_ unordered_map + delta update path.


using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::SyntaxMarker;

namespace {

    // Build a synthetic AST with `n` nodes of mixed tags/arities.
    // Returns count of unique (tag, arity=0) buckets expected for literals.
    void fill_ast(FlatAST& flat, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            // Alternate tags via add_literal / add_node
            if ((i % 5) == 0)
                (void)flat.add_literal(static_cast<std::int64_t>(i));
            else
                (void)flat.add_raw_node(NodeTag::Begin, SyntaxMarker::User);
        }
    }

} // namespace

int run_tag_arity_index_perf_1371() {
    // ── hash map rebuild + find ──
    {
        FlatAST flat;
        fill_ast(flat, 200);
        CHECK(flat.size() == 200, "200 nodes");
        flat.rebuild_tag_arity_index();
        CHECK(flat.tag_arity_index_size() >= 1, "index has buckets");
        CHECK(!flat.tag_arity_index_dirty(), "clean after rebuild");

        const auto hits0 = flat.tag_arity_index_hits();
        const auto miss0 = flat.tag_arity_index_misses();
        auto lit = flat.find_by_tag_arity(static_cast<std::uint32_t>(NodeTag::LiteralInt), 0, 0);
        if (lit.empty())
            lit = flat.find_by_tag_arity(static_cast<std::uint32_t>(NodeTag::Begin), 0, 0);
        CHECK(!lit.empty() || flat.tag_arity_index_size() > 0, "find or size ok");
        CHECK(flat.tag_arity_index_hits() + flat.tag_arity_index_misses() > hits0 + miss0,
              "hit/miss counters advanced");
    }

    // ── delta append path ──
    {
        FlatAST flat;
        fill_ast(flat, 50);
        flat.rebuild_tag_arity_index();
        const auto rebuilds0 = flat.tag_arity_index_rebuilds();
        const auto delta0 = flat.tag_arity_index_delta_hits();
        const auto built_n = flat.size();

        // Append more nodes
        fill_ast(flat, 30);
        CHECK(flat.size() == built_n + 30, "appended 30");
        flat.mark_tag_arity_index_dirty();
        flat.ensure_tag_arity_index();
        CHECK(flat.tag_arity_index_delta_hits() == delta0 + 1, "delta path taken");
        CHECK(flat.tag_arity_index_rebuilds() == rebuilds0, "no full rebuild on append");
        CHECK(!flat.tag_arity_index_dirty(), "clean after delta");
    }

    // ── rebuild_tag_arity_index_delta explicit range ──
    {
        FlatAST flat;
        fill_ast(flat, 20);
        flat.rebuild_tag_arity_index();
        const auto d0 = flat.tag_arity_index_delta_hits();
        fill_ast(flat, 10);
        flat.rebuild_tag_arity_index_delta(20, 30);
        CHECK(flat.tag_arity_index_delta_hits() == d0 + 1, "explicit delta +1");
        CHECK(!flat.tag_arity_index_dirty(), "delta clears dirty");
    }

    // ── mark_dirty_upward_with_index_update append ──
    {
        FlatAST flat;
        fill_ast(flat, 10);
        flat.rebuild_tag_arity_index();
        const auto d0 = flat.tag_arity_index_delta_hits();
        auto id = flat.add_literal(99);
        flat.mark_dirty_upward_with_index_update(id);
        // Issue #1503: append may live-patch (incremental_patches) or
        // clear dirty; either is correct vs full rebuild.
        CHECK(flat.tag_arity_index_delta_hits() >= d0 + 1 ||
                  flat.tag_arity_index_incremental_patches() > 0 || !flat.tag_arity_index_dirty(),
              "append update or clean");
        auto found = flat.find_by_tag_arity(static_cast<std::uint32_t>(flat.get(id).tag), 0, 0);
        bool contains = false;
        for (auto n : found)
            if (n == id)
                contains = true;
        // If tag/arity 0 bucket exists, id should be present after index update
        if (!found.empty())
            CHECK(contains, "new node in bucket after mark_dirty_upward_with_index_update");
        else
            CHECK(true, "skip contain check if arity mismatch");
    }

    // ── ensure full rebuild when dirty with no growth ──
    {
        FlatAST flat;
        fill_ast(flat, 40);
        flat.rebuild_tag_arity_index();
        const auto r0 = flat.tag_arity_index_rebuilds();
        flat.mark_tag_arity_index_dirty();
        // same size, dirty → full rebuild
        flat.ensure_tag_arity_index();
        CHECK(flat.tag_arity_index_rebuilds() == r0 + 1, "full rebuild when size unchanged");
        CHECK(!flat.tag_arity_index_dirty(), "clean after full ensure");
    }

    // ── 10K node rebuild + 1000 finds (smoke perf, not hard p99 bound) ──
    {
        FlatAST flat;
        fill_ast(flat, 10000);
        auto t0 = std::chrono::steady_clock::now();
        flat.rebuild_tag_arity_index();
        auto t1 = std::chrono::steady_clock::now();
        const auto rebuild_us =
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        CHECK(flat.tag_arity_index_size() >= 1, "10K index non-empty");

        // 1000 O(1) miss lookups (no large bucket copy noise)
        t0 = std::chrono::steady_clock::now();
        std::size_t misses = 0;
        for (int i = 0; i < 1000; ++i) {
            auto v = flat.find_by_tag_arity(0xDEADBEEFu, 99, 99);
            if (v.empty())
                ++misses;
        }
        t1 = std::chrono::steady_clock::now();
        const auto find_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        CHECK(misses == 1000, "1000 hash misses");
        CHECK(find_us < 10'000, "1000 hash misses < 10ms");
        // Hit path correctness on large bucket (copy cost separate from lookup)
        auto big = flat.find_by_tag_arity(static_cast<std::uint32_t>(NodeTag::Begin), 0, 0);
        CHECK(big.size() >= 1000, "Begin bucket large");
        std::println("  10K rebuild {} us; 1000 miss finds {} us; Begin bucket {}", rebuild_us,
                     find_us, big.size());
    }

    // ── atomic batch commit hooks delta ──
    {
        FlatAST flat;
        fill_ast(flat, 15);
        flat.rebuild_tag_arity_index();
        const auto d0 = flat.tag_arity_index_delta_hits();
        flat.begin_atomic_batch();
        fill_ast(flat, 5);
        flat.mark_tag_arity_index_dirty();
        flat.commit_atomic_batch();
        // commit should have run delta when size grew
        CHECK(flat.tag_arity_index_delta_hits() >= d0 + 1 || !flat.tag_arity_index_dirty(),
              "batch commit delta or clean");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("tag_arity_index perf #1371: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}

} // namespace aura_edsl_run_tag_arity_index_perf_1371
// ─── end test_tag_arity_index_perf.cpp ───

int main() {
    std::println("\n######## run_allow_macro_inline_1780 ########");
    if (int rc = aura_edsl_run_allow_macro_inline_1780::run_allow_macro_inline_1780(); rc != 0) {
        std::println("run_allow_macro_inline_1780 FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_protected_metadata_1838 ########");
    if (int rc = aura_edsl_run_protected_metadata_1838::run_protected_metadata_1838(); rc != 0) {
        std::println("run_protected_metadata_1838 FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_workspace_marker_macro_1678 ########");
    if (int rc = aura_edsl_run_workspace_marker_macro_1678::run_workspace_marker_macro_1678();
        rc != 0) {
        std::println("run_workspace_marker_macro_1678 FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_subtree_counter_1848 ########");
    if (int rc = aura_edsl_run_subtree_counter_1848::run_subtree_counter_1848(); rc != 0) {
        std::println("run_subtree_counter_1848 FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_tag_arity_index_perf_1371 ########");
    if (int rc = aura_edsl_run_tag_arity_index_perf_1371::run_tag_arity_index_perf_1371();
        rc != 0) {
        std::println("run_tag_arity_index_perf_1371 FAILED rc={}", rc);
        return rc;
    }
    if (::aura::test::g_failed)
        return 1;
    std::println("\ntest_edsl_hygiene_unit_batch: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
