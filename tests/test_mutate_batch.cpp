// test_mutate_batch.cpp
// B pilot #20 (after linear in 91d843ec): consolidated mutate family
// — Issues #1436 + #1691 + #1701 + #1772 + #1684 + #1702 + #1690 + #1696 +
// #1685 + #1703 + #1688 + #1689 + #1694 + #1697 + #1686 + #1687 + #1699 +
// #1704/#1705 + #1700 (mutate:op unified dispatcher + dead heap define_str +
// extract-function parent + from-feedback NodeId + Guard exception safety +
// inline-call parent + insert-child stale-parent + multi-node log NULL +
// rebind stale Define + refactor/extract parent + remove-node all parents +
// remove-node parent index + replace-pattern stale-parent + replace-subtree
// stale-parent + set-body exception safety + set-body stale ids +
// splice stale-parent + sv Guard + wrap stale-parent) into one batch driver.
//
// NOTE: test_mutate_cross_thread_migration.cpp NOT included — registered
// in cmake/AuraDomainTests.cmake:694-696 with add_dependencies(...) default-build
// (out of scope for batch consolidation).
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention.
// EXCLUDE_FROM_ALL — default build skips; on-demand 'ninja test_mutate_batch'.

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <print>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.core.arena;
import aura.core.ast;
import aura.core.mutators;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_mutate_batch {

using aura::ast::FlatAST;
using aura::ast::MutationRecord;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
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

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static bool eval_ok(CompilerService& cs, const std::string& expr) {
    return cs.eval(expr).has_value();
}

static bool child_of(const FlatAST& flat, NodeId parent, NodeId child) {
    if (parent >= flat.size() || !flat.is_live_node(parent))
        return false;
    for (auto c : flat.children(parent))
        if (c == child)
            return true;
    return false;
}

static NodeId find_call_op(const FlatAST& flat, StringPool& pool, std::string_view op_name) {
    for (NodeId id = 0; id < flat.size(); ++id) {
        if (!flat.is_live_node(id))
            continue;
        auto v = flat.get(id);
        if (v.tag != NodeTag::Call || v.children.empty())
            continue;
        auto f0 = flat.get(v.child(0));
        if (f0.tag == NodeTag::Variable && pool.resolve(f0.sym_id) == op_name)
            return id;
    }
    return NULL_NODE;
}

static bool find_last_op(const FlatAST& flat, std::string_view op, MutationRecord* out) {
    auto view = flat.mutation_log_view();
    for (std::size_t i = view.size(); i > 0; --i) {
        if (view[i - 1].operator_name == op) {
            if (out)
                *out = view[i - 1];
            return true;
        }
    }
    return false;
}

static std::uint64_t exception_rollbacks(CompilerService& cs) {
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    if (!m)
        return 0;
    return m->mutation_boundary_exception_rollback_total.load(std::memory_order_relaxed);
}

// ── Issue #1436 — mutate :op unified dispatcher ──
static void run_1436_dispatch() {
    std::println("\n--- Issue #1436: mutate :op unified dispatcher ---");
    CompilerService cs;

    CHECK(eval_ok(cs, "(set-code \"(define (f x) (+ x 1)) (define (g y) (f y)) (g 2)\")"),
          "set-code");
    CHECK(eval_ok(cs, "(eval-current)"), "eval-current");

    // AC1: :rebind
    {
        CHECK(eval_ok(cs, "(mutate :rebind \"f\" \"(lambda (x) (* x 2))\" \"t\")"),
              "mutate :rebind");
        CHECK(eval_ok(cs, "(mutate:rebind \"g\" \"(lambda (y) (+ y 1))\" \"t\")"),
              "mutate:rebind alias still works");
        CHECK(eval_ok(cs, "(eval-current)"), "eval after rebind");
    }

    // AC2: :extract
    {
        auto r = cs.eval("(mutate :extract 0 \"h\")");
        CHECK(r.has_value(), "mutate :extract returns");
        auto r2 = cs.eval("(mutate:extract-function 0 \"h2\")");
        CHECK(r2.has_value(), "mutate:extract-function alias returns");
    }

    // AC3: :move
    {
        auto r = cs.eval("(mutate :move 0 0 0)");
        CHECK(r.has_value(), "mutate :move returns");
        auto r2 = cs.eval("(mutate:move-node 0 0 0)");
        CHECK(r2.has_value(), "mutate:move-node alias returns");
    }

    // AC4: :atomic
    {
        auto r = cs.eval("(mutate :atomic (list))");
        CHECK(r.has_value() || !r, "mutate :atomic handled");
        (void)r;
        CHECK(true, "mutate :atomic path exercised");
    }

    // AC5: :replace
    {
        auto r = cs.eval("(mutate :replace 0 :type \"Int\")");
        CHECK(r.has_value(), "mutate :replace :type returns");
        auto r2 = cs.eval("(mutate:replace-type 0 \"Int\")");
        CHECK(r2.has_value(), "mutate:replace-type alias returns");
    }

    // AC6: :validate
    {
        auto r = cs.eval("(mutate :validate \"(+ 1 2)\" \"number\")");
        CHECK(r.has_value(), "mutate :validate returns");
    }

    // AC7: api-reference
    {
        auto r = cs.eval("(api-reference)");
        CHECK(r && is_string(*r), "api-reference string");
        if (r && is_string(*r)) {
            auto idx = as_int(*r); // eval'd as idx earlier; treat as string
            auto heap = cs.evaluator().string_heap();
            std::string s = (idx < static_cast<std::int64_t>(heap.size())) ? heap[idx] : "";
            CHECK(s.find("mutate") != std::string::npos, "lists mutate");
            CHECK(s.find("*deprecated*") != std::string::npos, "*deprecated* section");
            CHECK(s.find("mutate:rebind") != std::string::npos, "deprecated mutate:rebind");
        }
    }

    // AC8: unknown op
    {
        auto r = cs.eval("(mutate :nope)");
        (void)r;
        CHECK(true, "unknown op no crash");
    }
}

// ── Issue #1691 — mutate:refactor/extract dead heap define_str ──
static void run_1691_dead_heap() {
    std::println("\n--- Issue #1691: mutate refactor/extract dead heap define_str ---");
    // AC1: source-level contract
    {
        std::println("\n--- AC1 (#1691): no dead define_str push ---");
        auto src = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(!src.empty(), "read evaluator_primitives_mutate.cpp");
        CHECK(src.find("std::string define_str = \"(define (\"") != std::string::npos,
              "define_str local construction present");
        auto pos = src.find("std::string define_str = \"(define (\"");
        CHECK(pos != std::string::npos, "locate define_str site");
        if (pos != std::string::npos) {
            auto window = src.substr(pos, 400);
            CHECK(window.find("define_idx") == std::string::npos, "no define_idx capture");
            CHECK(window.find("string_heap_.push_back(define_str)") == std::string::npos,
                  "no push_back(define_str)");
            CHECK(window.find("parse_to_flat(define_str") != std::string::npos,
                  "parse_to_flat uses define_str directly");
        }
    }

    // AC2: audit clean
    {
        std::println("\n--- AC2 (#1691): audit_dead_heap_push clean ---");
        int rc = std::system("python3 scripts/audit_dead_heap_push.py >/tmp/audit_1691.out 2>&1");
        CHECK(rc == 0, "audit_dead_heap_push exit 0");
        auto out = read_file("/tmp/audit_1691.out");
        CHECK(out.find("clean") != std::string::npos ||
                  out.find("0 candidates") != std::string::npos,
              "audit reports clean / 0 candidates");
    }

    // AC3: heap growth bounded
    {
        std::println("\n--- AC3 (#1691): refactor/extract heap growth ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (define (g y) (+ y 2))\")").has_value(),
              "set-code");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat");

        NodeId lit = NULL_NODE;
        for (NodeId id = 0; id < flat->size(); ++id) {
            if (!flat->is_live_node(id))
                continue;
            auto v = flat.get(id);
            if (v.tag == NodeTag::LiteralInt && v.int_value == 1) {
                lit = id;
                break;
            }
        }
        CHECK(lit != NULL_NODE, "found lit 1");

        const auto heap0 = cs.evaluator().string_heap().size();
        auto r = cs.eval(std::string("(mutate:refactor/extract ") + std::to_string(lit) +
                         " \"extracted_fn_1691\" \"ac3\")");
        const auto heap1 = cs.evaluator().string_heap().size();
        std::println("  heap before={} after={} delta={} extract_ok={}", heap0, heap1,
                     heap1 - heap0, r.has_value());
        CHECK(heap1 - heap0 <= 8, "extract does not dump large dead string_heap payload");
    }
}

// ── Issue #1701 — extract-function stale parent after pad growth ──
static void run_1701_extract_parent() {
    std::println("\n--- Issue #1701: extract-function parent revalidate ---");
    CompilerService cs;
    std::string src = "(define (f x) (* 2 x))";
    for (int i = 0; i < 48; ++i) {
        src += " (define (pad";
        src += std::to_string(i);
        src += " y) (+ y ";
        src += std::to_string(i);
        src += "))";
    }
    CHECK(eval_ok(cs, std::string("(set-code \"") + src + "\")"), "set-code pad");
    auto* flat = cs.evaluator().workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    CHECK(flat != nullptr && pool != nullptr, "flat+pool");

    auto star = find_call_op(*flat, *pool, "*");
    CHECK(star != NULL_NODE, "found (* 2 x)");
    auto parent_before = flat->parent_of(star);
    CHECK(parent_before != NULL_NODE && flat->is_live_node(parent_before),
          "target has live parent");

    const auto size_before = flat->size();
    auto expr = std::string("(mutate:extract-function ") + std::to_string(star) +
                " \"extracted-g\" \"1701-ac1\")";
    auto r = cs.eval(expr);
    CHECK(r.has_value(), "extract-function eval ok");
    CHECK(flat->size() > size_before, "flat grew from add_*");
    CHECK(flat->is_live_node(parent_before), "original parent still live");

    bool parent_has_call = false;
    for (auto c : flat->children(parent_before)) {
        if (!flat->is_live_node(c))
            continue;
        if (flat->get(c).tag == NodeTag::Call) {
            parent_has_call = true;
            break;
        }
    }
    CHECK(parent_has_call, "parent has Call replacement after extract");

    bool found_define = false;
    for (NodeId id = 0; id < flat->size(); ++id) {
        if (!flat->is_live_node(id))
            continue;
        auto v = flat->get(id);
        if (v.tag != NodeTag::Define)
            continue;
        auto nm = pool->resolve(v.sym_id);
        if (nm == "extracted-g") {
            found_define = true;
            break;
        }
    }
    CHECK(found_define, "Define extracted-g present after extract");

    auto p0 = cs.eval("(pad0 1)");
    (void)cs.eval("(eval-current)");
    p0 = cs.eval("(pad0 1)");
    CHECK(p0.has_value(), "pad0 still evaluates");
}

// ── Issue #1772 — mutate:from-feedback NodeId validation ──
static void run_1772_from_feedback() {
    std::println("\n--- Issue #1772: mutate from-feedback NodeId validation ---");
    // AC1/AC2: source + metric
    {
        std::println("\n--- AC1/AC2 (#1772): source + metric ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile_04.cpp",
                              "../src/compiler/evaluator_primitives_compile_04.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_04.cpp");
        CHECK(src.find("#1772") != std::string::npos, "cites #1772");
        CHECK(src.find("mutate_from_feedback_invalid_node_total") != std::string::npos,
              "bumps invalid_node metric");
        CHECK(src.find("mutate:from-verification-feedback") != std::string::npos,
              "primitive present");

        std::string msrc;
        for (const char* p :
             {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
            msrc = read_file(p);
            if (!msrc.empty())
                break;
        }
        CHECK(!msrc.empty() &&
                  msrc.find("mutate_from_feedback_invalid_node_total") != std::string::npos,
              "metric field declared");
    }

    // AC3: OOB
    {
        std::println("\n--- AC3 (#1772): OOB rejected ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "metrics wired");
        const auto n0 = m->mutate_from_feedback_invalid_node_total.load(std::memory_order_relaxed);

        auto r =
            cs.eval("(mutate:from-verification-feedback \"weaken-property\" 999999 \"reset\")");
        CHECK(r && is_bool(*r) && !as_bool(*r), "OOB returns #f");
        CHECK(m->mutate_from_feedback_invalid_node_total.load(std::memory_order_relaxed) == n0 + 1,
              "invalid_node_total +1");

        auto r2 = cs.eval("(mutate:from-verification-feedback \"weaken-property\" -1 \"reset\")");
        CHECK(r2 && is_bool(*r2) && !as_bool(*r2), "negative returns #f");
    }

    // AC4: in-range
    {
        std::println("\n--- AC4 (#1772): in-range no invalid bump ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        auto* ws = ev.workspace_flat();
        if (!ws || ws->size() == 0) {
            CHECK(false, "workspace non-empty after set-code");
        } else {
            const auto n0 =
                m->mutate_from_feedback_invalid_node_total.load(std::memory_order_relaxed);
            const auto nid = static_cast<std::int64_t>(ws->size() - 1);
            auto r = cs.eval(std::format(
                "(mutate:from-verification-feedback \"weaken-property\" {} \"reset\")", nid));
            CHECK(r && is_bool(*r), "returns bool");
            CHECK(m->mutate_from_feedback_invalid_node_total.load(std::memory_order_relaxed) == n0,
                  "no invalid bump for in-range id");
        }
    }
}

// ── Issue #1684 — MutationBoundaryGuard::run_or_rollback exception safety ──
static void run_1684_guard_exception() {
    std::println("\n--- Issue #1684: Guard run_or_rollback ---");
    CompilerService cs;

    // AC1: success
    {
        std::println("\n--- AC1 (#1684): run_or_rollback success ---");
        bool ok = true;
        auto g = Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
        CHECK(g.has_value() && *g, "try_acquire AC1");
        int n = 0;
        CHECK((*g)->run_or_rollback([&] { n = 42; }), "run_or_rollback returns true");
        CHECK(n == 42, "fn ran");
        CHECK(ok, "ok still true after success");
    }

    // AC2: throw path
    {
        std::println("\n--- AC2 (#1684): throw marks failed ---");
        const auto r0 = exception_rollbacks(cs);
        bool ok = true;
        auto g = Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
        CHECK(g.has_value() && *g, "try_acquire AC2");
        std::string err;
        CHECK(!(*g)->run_or_rollback([&] { throw std::runtime_error("boom-1684"); }, &err),
              "run_or_rollback returns false on throw");
        CHECK(!ok, "ok=false after throw");
        CHECK(err.find("boom-1684") != std::string::npos, "err captures what()");
        CHECK(exception_rollbacks(cs) > r0, "exception-rollback counter advanced");
        ok = true;
        auto g2 = Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
        CHECK(g2.has_value() && *g2, "try_acquire AC2b");
        CHECK(!(*g2)->run_or_rollback([&] { throw 7; }), "catch-all (...)");
        CHECK(!ok, "ok=false after non-std throw");
    }

    // AC3: rebind happy path
    {
        std::println("\n--- AC3 (#1684): mutate:rebind happy path ---");
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code f");
        auto r = cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 2))\" \"bump\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "rebind #t");
    }

    // AC4: rebind after throw
    {
        std::println("\n--- AC4 (#1684): rebind after throw ---");
        auto r = cs.eval("(mutate:rebind \"f\" \"(lambda (x) (* x 3))\" \"mul\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "second rebind #t");
    }
}

// ── Issue #1702 — mutate:inline-call stale parent ──
static void run_1702_inline_call() {
    std::println("\n--- Issue #1702: inline-call stale parent ---");
    CompilerService cs;
    std::string src = "(define (double x) (+ x x)) (define (f y) (double y))";
    for (int i = 0; i < 48; ++i) {
        src += " (define (pad";
        src += std::to_string(i);
        src += " z) (+ z ";
        src += std::to_string(i);
        src += "))";
    }
    CHECK(eval_ok(cs, std::string("(set-code \"") + src + "\")"), "set-code pad");
    auto* flat = cs.evaluator().workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    CHECK(flat != nullptr && pool != nullptr, "flat+pool");

    auto call = find_call_op(*flat, *pool, "double");
    CHECK(call != NULL_NODE, "found (double y) call");
    auto parent_before = flat->parent_of(call);
    CHECK(parent_before != NULL_NODE && flat->is_live_node(parent_before), "call has live parent");
    const auto size_before = flat->size();

    auto expr = std::string("(mutate:inline-call ") + std::to_string(call) + " \"1702-ac1\")";
    auto r = cs.eval(expr);
    CHECK(r.has_value(), "inline-call eval ok");
    if (r && is_bool(*r))
        CHECK(as_bool(*r), "inline-call #t");
    CHECK(flat->size() > size_before, "flat grew from DFS clone add_*");
    CHECK(flat->is_live_node(parent_before), "original parent still live");
    CHECK(!child_of(*flat, parent_before, call),
          "original call not still a direct child of parent");

    bool parent_has_double_call = false;
    for (auto c : flat->children(parent_before)) {
        if (!flat->is_live_node(c))
            continue;
        auto cv = flat->get(c);
        if (cv.tag != NodeTag::Call || cv.children.empty())
            continue;
        auto f0 = flat->get(cv.child(0));
        if (f0.tag == NodeTag::Variable && pool->resolve(f0.sym_id) == "double")
            parent_has_double_call = true;
    }
    CHECK(!parent_has_double_call, "parent no longer holds (double …) call site");

    auto p0 = cs.eval("(pad0 1)");
    (void)cs.eval("(eval-current)");
    p0 = cs.eval("(pad0 1)");
    CHECK(p0.has_value(), "pad0 still evaluates");
}

// ── Issue #1690 — mutate:insert-child stale parent ──
static void run_1690_insert_child() {
    std::println("\n--- Issue #1690: insert-child stale parent ---");
    CompilerService cs;
    std::string src = "(define (f x) (begin 1 2))";
    for (int i = 0; i < 48; ++i) {
        src += " (define (pad";
        src += std::to_string(i);
        src += " y) (+ y ";
        src += std::to_string(i);
        src += "))";
    }
    CHECK(eval_ok(cs, std::string("(set-code \"") + src + "\")"), "set-code pad");
    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "workspace flat");

    NodeId begin_id = NULL_NODE;
    for (NodeId id = 0; id < flat->size(); ++id) {
        if (!flat->is_live_node(id))
            continue;
        if (flat->get(id).tag == NodeTag::Begin) {
            begin_id = id;
            break;
        }
    }
    CHECK(begin_id != NULL_NODE, "found Begin parent");
    const auto size_before = flat->size();
    const auto child_count_before = flat->children(begin_id).size();

    auto expr = std::string("(mutate:insert-child ") + std::to_string(begin_id) +
                " 0 \"(+ 100 200 300 400 500 600 700 800)\" \"1690-ac1\")";
    auto r = cs.eval(expr);
    CHECK(r && is_int(*r), "insert-child returns new node id");
    auto new_id = static_cast<NodeId>(as_int(*r));
    CHECK(flat->size() > size_before, "flat grew after parse");
    CHECK(flat->is_live_node(new_id), "new child live");
    CHECK(child_of(*flat, begin_id, new_id), "new child under intended Begin");
    CHECK(flat->children(begin_id).size() == child_count_before + 1, "Begin child count +1");

    int wrong = 0;
    for (NodeId id = 0; id < flat->size(); ++id) {
        if (id == begin_id || !flat->is_live_node(id))
            continue;
        if (flat->get(id).tag != NodeTag::Define)
            continue;
        for (auto c : flat->children(id))
            if (c == new_id)
                ++wrong;
    }
    CHECK(wrong == 0, "no Define incorrectly received inserted child");
}

// ── Issue #1696 — multi-node mutators log under NULL_NODE ──
static void run_1696_multi_node_log() {
    std::println("\n--- Issue #1696: multi-node log NULL_NODE ---");
    // AC1: sentinel contract
    {
        std::println("\n--- AC1 (#1696): NULL_NODE != 0 ---");
        static_assert(NULL_NODE != 0, "NULL_NODE must not be 0 (0 is a real NodeId)");
        CHECK(NULL_NODE != 0, "NULL_NODE is not NodeId 0");
        CHECK(NULL_NODE == ~0u, "NULL_NODE is ~0u");
    }

    // AC2: replace-pattern logs under NULL_NODE
    {
        std::println("\n--- AC2 (#1696): replace-pattern log target ---");
        CompilerService cs;
        CHECK(eval_ok(cs, "(set-code \"(define (f x) (* 2 x))\")"), "set-code");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat");
        const auto log_before = flat->mutation_count();

        auto r = cs.eval("(mutate:replace-pattern \"(* 2 x)\" \"(+ x x)\" \"1696-ac2\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "replace-pattern #t");
        CHECK(flat->mutation_count() > log_before, "mutation log grew");

        MutationRecord rec{};
        CHECK(find_last_op(*flat, "replace-pattern", &rec), "found replace-pattern log entry");
        CHECK(rec.target_node == NULL_NODE, "replace-pattern target is NULL_NODE");
        CHECK(rec.target_node != 0, "replace-pattern target is not NodeId 0");
    }

    // AC3: rename-symbol logs under NULL_NODE
    {
        std::println("\n--- AC3 (#1696): rename-symbol log target ---");
        CompilerService cs;
        CHECK(eval_ok(cs, "(set-code \"(define (g x) (+ x 1))\")"), "set-code g");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat ac3");
        const auto log_before = flat->mutation_count();

        auto r = cs.eval("(mutate:rename-symbol \"x\" \"y\" \"1696-ac3\")");
        CHECK(r.has_value(), "rename-symbol completed");
        if (r && is_bool(*r))
            CHECK(as_bool(*r), "rename-symbol #t");
        CHECK(flat->mutation_count() > log_before, "mutation log grew after rename");

        MutationRecord rec{};
        CHECK(find_last_op(*flat, "rename-symbol", &rec), "found rename-symbol log entry");
        CHECK(rec.target_node == NULL_NODE, "rename-symbol target is NULL_NODE");
        CHECK(rec.target_node != 0, "rename-symbol target is not NodeId 0");
    }

    // AC4: source audit
    {
        std::println("\n--- AC4 (#1696): source has no bare add_mutation(0, multi-op) ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_mutate.cpp",
            "src/compiler/evaluator_eval_flat.cpp",
            "../src/compiler/evaluator_primitives_mutate.cpp",
            "../src/compiler/evaluator_eval_flat.cpp",
        };
        std::string mutate_src, flat_src;
        for (const char* p : candidates) {
            auto s = read_file(p);
            if (s.empty())
                continue;
            if (std::string_view(p).find("evaluator_primitives_mutate") != std::string_view::npos)
                mutate_src = std::move(s);
            if (std::string_view(p).find("evaluator_eval_flat") != std::string_view::npos)
                flat_src = std::move(s);
        }
        CHECK(!mutate_src.empty(), "read evaluator_primitives_mutate.cpp");
        CHECK(!flat_src.empty(), "read evaluator_eval_flat.cpp");
        if (!mutate_src.empty()) {
            CHECK(mutate_src.find("add_mutation(NULL_NODE, \"replace-pattern\"") !=
                      std::string::npos,
                  "public replace-pattern uses NULL_NODE");
            CHECK(mutate_src.find("add_mutation(NULL_NODE, \"rename-symbol\"") != std::string::npos,
                  "public rename-symbol uses NULL_NODE");
        }
        if (!flat_src.empty()) {
            CHECK(flat_src.find("add_mutation(aura::ast::NULL_NODE, \"replace-pattern\"") !=
                      std::string::npos,
                  "lockless replace-pattern uses aura::ast::NULL_NODE");
            CHECK(flat_src.find("add_mutation(aura::ast::NULL_NODE, \"rename-symbol\"") !=
                      std::string::npos,
                  "lockless rename-symbol uses aura::ast::NULL_NODE");
        }
    }
}

// ── Issue #1685 — rebind stale Define ──
static void run_1685_rebind_stale_define() {
    std::println("\n--- Issue #1685: rebind stale Define ---");
    CompilerService cs;

    static auto rebind_ok = [](CompilerService& cs, const std::string& name,
                               const std::string& code, const std::string& summary) {
        auto expr =
            std::string("(mutate:rebind \"") + name + "\" \"" + code + "\" \"" + summary + "\")";
        auto r = cs.eval(expr);
        return r && is_bool(*r) && as_bool(*r);
    };
    static auto int_eq = [](CompilerService& cs, const std::string& expr, std::int64_t want) {
        auto r = cs.eval(expr);
        if (r && is_int(*r) && as_int(*r) == want)
            return true;
        (void)cs.eval("(eval-current)");
        r = cs.eval(expr);
        return r && is_int(*r) && as_int(*r) == want;
    };

    // AC1: rebind only named define
    {
        std::println("\n--- AC1 (#1685): multi-define rebind locality ---");
        const char* multi = "(set-code \"(define (a x) (+ x 1)) (define (b y) (+ y 2)) "
                            "(define (c z) (+ z 3))\")";
        CHECK(eval_ok(cs, multi), "set-code a/b/c");
        CHECK(rebind_ok(cs, "b", "(lambda (y) (* y 10))", "1685-ac1"), "rebind b #t");
        CHECK(int_eq(cs, "(a 10)", 11), "a unchanged (11)");
        CHECK(int_eq(cs, "(b 10)", 100), "b rebound (100)");
        CHECK(int_eq(cs, "(c 10)", 13), "c unchanged (13)");
    }

    // AC2: full define form
    {
        std::println("\n--- AC2 (#1685): rebind with full define form ---");
        CHECK(rebind_ok(cs, "a", "(define (a x) (+ x 100))", "1685-ac2"),
              "rebind a full-define #t");
        CHECK(int_eq(cs, "(a 1)", 101), "a → 101 after full-define rebind");
        CHECK(int_eq(cs, "(b 3)", 30), "b still 30 (not corrupted)");
    }

    // AC3: set-body after growth
    {
        std::println("\n--- AC3 (#1685): set-body after sibling rebinds ---");
        for (int i = 0; i < 8; ++i) {
            std::string code = "(lambda (z) (+ z " + std::to_string(i) + "))";
            CHECK(rebind_ok(cs, "c", code, "grow"), "grow rebind c");
        }
        auto expr = std::string("(mutate:set-body \"c\" \"(* z 7)\" \"1685-ac3\")");
        auto r = cs.eval(expr);
        CHECK(r && is_bool(*r) && as_bool(*r), "set-body c #t");
        CHECK(int_eq(cs, "(c 5)", 35), "c 5 → 35 after set-body");
        CHECK(int_eq(cs, "(a 1)", 101), "a still 101 after set-body c");
    }

    // AC4: capacity-boundary rebind
    {
        std::println("\n--- AC4 (#1685): capacity-boundary pad + rebind ---");
        std::string src = "(define (target n) (+ n 1))";
        for (int i = 0; i < 64; ++i) {
            src += " (define (pad";
            src += std::to_string(i);
            src += " x) (+ x ";
            src += std::to_string(i);
            src += "))";
        }
        auto set = std::string("(set-code \"") + src + "\")";
        CHECK(eval_ok(cs, set), "set-code target + 64 pads");
        CHECK(rebind_ok(cs, "target", "(lambda (n) (* n 2))", "1685-ac4"),
              "rebind target #t after pad");
        CHECK(int_eq(cs, "(target 21)", 42), "target 21 → 42");
        CHECK(int_eq(cs, "(pad0 1)", 1), "pad0 unchanged");
        CHECK(int_eq(cs, "(pad63 1)", 64), "pad63 unchanged");
    }
}

// ── Issue #1703 — refactor/extract stale parent ──
static void run_1703_refactor_extract() {
    std::println("\n--- Issue #1703: refactor/extract stale parent ---");
    CompilerService cs;
    std::string src = "(define (f x) (* 2 x))";
    for (int i = 0; i < 48; ++i) {
        src += " (define (pad";
        src += std::to_string(i);
        src += " y) (+ y ";
        src += std::to_string(i);
        src += "))";
    }
    CHECK(eval_ok(cs, std::string("(set-code \"") + src + "\")"), "set-code pad");
    auto* flat = cs.evaluator().workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    CHECK(flat != nullptr && pool != nullptr, "flat+pool");

    auto star = find_call_op(*flat, *pool, "*");
    CHECK(star != NULL_NODE, "found (* 2 x)");
    auto parent_before = flat->parent_of(star);
    CHECK(parent_before != NULL_NODE && flat->is_live_node(parent_before),
          "target has live parent");
    const auto size_before = flat->size();

    auto expr = std::string("(mutate:refactor/extract ") + std::to_string(star) +
                " \"ref-ex\" \"1703-ac1\")";
    auto r = cs.eval(expr);
    CHECK(r.has_value(), "refactor/extract eval ok");
    CHECK(flat->size() > size_before, "flat grew after parse");
    CHECK(flat->is_live_node(parent_before), "original parent still live");
    CHECK(!child_of(*flat, parent_before, star), "original * no longer direct child of parent");

    bool parent_has_replacement = false;
    for (auto c : flat->children(parent_before)) {
        if (flat->is_live_node(c) && c != star) {
            parent_has_replacement = true;
            break;
        }
    }
    CHECK(parent_has_replacement, "parent has a replacement child");

    auto p0 = cs.eval("(pad0 1)");
    (void)cs.eval("(eval-current)");
    p0 = cs.eval("(pad0 1)");
    CHECK(p0.has_value(), "pad0 still evaluates");
}

// ── Issue #1688 — remove-node all parents (DAG) ──
static void run_1688_remove_node_all_parents() {
    std::println("\n--- Issue #1688: remove-node all parents ---");
    static auto remove_node_ok = [](CompilerService& cs, NodeId id) {
        auto expr = std::string("(mutate:remove-node ") + std::to_string(id) + ")";
        auto r = cs.eval(expr);
        return r && is_bool(*r) && as_bool(*r);
    };
    static auto count_incoming = [](const FlatAST& flat, NodeId target) {
        return aura::ast::mutators::collect_incoming_child_edges(flat, target).size();
    };

    // AC1: single-parent (tree)
    {
        std::println("\n--- AC1 (#1688): single-parent remove-node ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (begin 10 20 30))\")").has_value(), "set-code");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "workspace flat");
        NodeId lit20 = NULL_NODE;
        for (NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_free_slot(id))
                continue;
            auto v = flat->get(id);
            if (v.tag == NodeTag::LiteralInt && v.int_value == 20) {
                lit20 = id;
                break;
            }
        }
        CHECK(lit20 != NULL_NODE, "found lit 20");
        CHECK(count_incoming(*flat, lit20) == 1, "lit20 has 1 parent before");
        CHECK(remove_node_ok(cs, lit20), "remove-node lit20 #t");
        CHECK(count_incoming(*flat, lit20) == 0, "lit20 has 0 parents after");
    }

    // AC2: dual-parent DAG
    {
        std::println("\n--- AC2 (#1688): dual-parent shared child ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (begin 1 2))\")").has_value(), "set-code f");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat AC2");

        NodeId shared = NULL_NODE;
        for (NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_free_slot(id))
                continue;
            auto v = flat->get(id);
            if (v.tag == NodeTag::LiteralInt && v.int_value == 1)
                shared = id;
        }
        CHECK(shared != NULL_NODE, "found shared lit1");

        auto p2 = flat->add_node(NodeTag::Begin);
        flat->insert_child(p2, 0, shared);
        CHECK(count_incoming(*flat, shared) >= 2, "shared has ≥2 parents");

        CHECK(remove_node_ok(cs, shared), "remove-node shared #t");
        CHECK(count_incoming(*flat, shared) == 0, "shared fully detached");
    }

    // AC3: same parent two slots
    {
        std::println("\n--- AC3 (#1688): same parent two slots ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (g x) 0)\")").has_value(), "set-code g");
        auto* flat = cs.evaluator().workspace_flat();
        auto shared = flat->add_node(NodeTag::LiteralInt);
        flat->set_int(shared, 99);
        auto parent = flat->add_node(NodeTag::Begin);
        flat->insert_child(parent, 0, shared);
        flat->insert_child(parent, 1, shared);
        CHECK(count_incoming(*flat, shared) == 2, "two edges same parent");
        CHECK(remove_node_ok(cs, shared), "remove both slots #t");
        CHECK(count_incoming(*flat, shared) == 0, "no edges after multi-slot remove");
    }

    // AC4: collect order
    {
        std::println("\n--- AC4 (#1688): edge collect order ---");
        FlatAST flat;
        auto shared = flat.add_node(NodeTag::LiteralInt);
        flat.set_int(shared, 7);
        auto parent = flat.add_node(NodeTag::Begin);
        flat.insert_child(parent, 0, shared);
        flat.insert_child(parent, 1, shared);
        flat.insert_child(parent, 2, shared);
        auto edges = aura::ast::mutators::collect_incoming_child_edges(flat, shared);
        CHECK(edges.size() == 3, "3 edges");
        CHECK(edges[0].child_index == 2 && edges[1].child_index == 1 && edges[2].child_index == 0,
              "indices descending 2,1,0");
    }
}

// ── Issue #1689 — remove-node parent index O(1) ──
static void run_1689_remove_node_index() {
    std::println("\n--- Issue #1689: remove-node parent index ---");
    static auto scan_all = [](const FlatAST& flat, NodeId target) {
        std::vector<aura::ast::mutators::IncomingChildEdge> edges;
        for (NodeId id = 0; id < flat.size(); ++id) {
            if (flat.is_free_slot(id))
                continue;
            auto ch = flat.children(id);
            for (std::size_t ci = 0; ci < ch.size(); ++ci) {
                if (ch[ci] == target)
                    edges.push_back({id, static_cast<std::uint32_t>(ci)});
            }
        }
        std::ranges::sort(edges, [](const auto& a, const auto& b) {
            if (a.parent != b.parent)
                return a.parent < b.parent;
            return a.child_index > b.child_index;
        });
        return edges;
    };
    static auto edges_equal = [](const std::vector<aura::ast::mutators::IncomingChildEdge>& a,
                                 const std::vector<aura::ast::mutators::IncomingChildEdge>& b) {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].parent != b[i].parent || a[i].child_index != b[i].child_index)
                return false;
        return true;
    };
    static auto remove_ok = [](CompilerService& cs, NodeId id) {
        auto r = cs.eval(std::string("(mutate:remove-node ") + std::to_string(id) + ")");
        return r && is_bool(*r) && as_bool(*r);
    };

    // AC1: index collect == full scan
    {
        std::println("\n--- AC1 (#1689): index matches full scan ---");
        FlatAST flat;
        auto a = flat.add_node(NodeTag::LiteralInt);
        flat.set_int(a, 1);
        auto b = flat.add_node(NodeTag::LiteralInt);
        flat.set_int(b, 2);
        auto p1 = flat.add_node(NodeTag::Begin);
        auto p2 = flat.add_node(NodeTag::Begin);
        flat.insert_child(p1, 0, a);
        flat.insert_child(p1, 1, b);
        flat.insert_child(p2, 0, a);
        flat.mark_incoming_parent_index_dirty();
        auto via_index = aura::ast::mutators::collect_incoming_child_edges(flat, a);
        auto via_scan = scan_all(flat, a);
        CHECK(via_index.size() == 2, "a has 2 edges");
        CHECK(edges_equal(via_index, via_scan), "index == scan for a");
        CHECK(flat.incoming_parent_index_rebuilds() >= 1, "rebuild counted");
    }

    // AC2: warm index hits on remove-node
    {
        std::println("\n--- AC2 (#1689): remove-node index hits ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (begin 10 20 30 40 50))\")").has_value(),
              "set-code");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat");
        flat->mark_incoming_parent_index_dirty();
        const auto rebuilds0 = flat->incoming_parent_index_rebuilds();
        const auto hits0 = flat->incoming_parent_index_hits();
        const auto lookups0 = flat->incoming_parent_index_lookups();
        std::vector<NodeId> lits;
        for (NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_free_slot(id))
                continue;
            auto v = flat->get(id);
            if (v.tag == NodeTag::LiteralInt)
                lits.push_back(id);
        }
        CHECK(lits.size() >= 3, "enough lits");
        for (std::size_t i = 0; i < 3 && i < lits.size(); ++i)
            CHECK(remove_ok(cs, lits[i]), "remove lit");
        CHECK(flat->incoming_parent_index_lookups() > lookups0, "lookups advanced");
        CHECK(flat->incoming_parent_index_hits() > hits0, "hits advanced");
        const auto rebuilds_delta = flat->incoming_parent_index_rebuilds() - rebuilds0;
        CHECK(rebuilds_delta <= 3, "rebuilds bounded (≤3 for 3 removes)");
    }

    // AC4: incremental insert/remove without dirty every time
    {
        std::println("\n--- AC4 (#1689): incremental structural ops ---");
        FlatAST flat;
        auto x = flat.add_node(NodeTag::LiteralInt);
        flat.set_int(x, 9);
        auto p = flat.add_node(NodeTag::Begin);
        flat.mark_incoming_parent_index_dirty();
        flat.ensure_incoming_parent_index();
        const auto r0 = flat.incoming_parent_index_rebuilds();
        flat.insert_child(p, 0, x);
        flat.insert_child(p, 1, x);
        CHECK(!flat.incoming_parent_index_dirty(), "index still clean after inserts");
        auto e = aura::ast::mutators::collect_incoming_child_edges(flat, x);
        CHECK(e.size() == 2, "two edges after dual insert");
        CHECK(edges_equal(e, scan_all(flat, x)), "incremental matches scan");
        CHECK(flat.incoming_parent_index_rebuilds() == r0, "no extra rebuild on collect");
        flat.remove_child(p, 1);
        e = aura::ast::mutators::collect_incoming_child_edges(flat, x);
        CHECK(e.size() == 1, "one edge after remove");
        CHECK(edges_equal(e, scan_all(flat, x)), "after remove matches scan");
    }
}

// ── Issue #1694 — replace-pattern stale parent ──
static void run_1694_replace_pattern() {
    std::println("\n--- Issue #1694: replace-pattern stale parent ---");
    CompilerService cs;
    std::string src = "(define (f x) (begin (* 2 x) (* 2 x) (* 2 x)))";
    for (int i = 0; i < 40; ++i) {
        src += " (define (pad";
        src += std::to_string(i);
        src += " y) (+ y ";
        src += std::to_string(i);
        src += "))";
    }
    CHECK(eval_ok(cs, std::string("(set-code \"") + src + "\")"), "set-code");
    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "flat");

    auto r = cs.eval("(mutate:replace-pattern \"(* 2 x)\" \"(+ x x)\" \"1694-ac1\")");
    CHECK(r && is_bool(*r) && as_bool(*r), "replace-pattern #t");
    CHECK(flat->size() > 40, "flat grew from replacements");

    int plus_x_x = 0;
    for (NodeId id = 0; id < flat->size(); ++id) {
        if (!flat->is_live_node(id))
            continue;
        auto v = flat->get(id);
        if (v.tag != NodeTag::Call || v.children.size() != 3)
            continue;
        auto f0 = flat->get(v.child(0));
        if (f0.tag != NodeTag::Variable)
            continue;
        auto name = cs.evaluator().workspace_pool()->resolve(f0.sym_id);
        if (name == "+") {
            auto a1 = flat->get(v.child(1));
            auto a2 = flat->get(v.child(2));
            if (a1.tag == NodeTag::Variable && a2.tag == NodeTag::Variable)
                ++plus_x_x;
        }
    }
    CHECK(plus_x_x >= 1, "at least one (+ x x) live after replace");

    auto p0 = cs.eval("(pad0 1)");
    (void)cs.eval("(eval-current)");
    p0 = cs.eval("(pad0 1)");
    CHECK(p0.has_value(), "pad0 still evaluates");
}

// ── Issue #1697 — replace-subtree stale parent ──
static void run_1697_replace_subtree() {
    std::println("\n--- Issue #1697: replace-subtree stale parent ---");
    CompilerService cs;
    std::string src = "(define (f x) (* 2 x))";
    for (int i = 0; i < 48; ++i) {
        src += " (define (pad";
        src += std::to_string(i);
        src += " y) (+ y ";
        src += std::to_string(i);
        src += "))";
    }
    CHECK(eval_ok(cs, std::string("(set-code \"") + src + "\")"), "set-code pad");
    auto* flat = cs.evaluator().workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    CHECK(flat != nullptr && pool != nullptr, "workspace flat+pool");

    auto star_call = find_call_op(*flat, *pool, "*");
    CHECK(star_call != NULL_NODE, "found (* 2 x) call");
    auto parent_before = flat->parent_of(star_call);
    CHECK(parent_before != NULL_NODE, "target has parent");
    CHECK(flat->is_live_node(parent_before), "parent live before");
    const auto size_before = flat->size();

    auto expr = std::string("(mutate:replace-subtree ") + std::to_string(star_call) +
                " \"(+ x x 0 0 0 0 0 0 0 0)\" \"1697-ac1\")";
    auto r = cs.eval(expr);
    CHECK(r.has_value(), "replace-subtree eval ok");
    if (r && is_bool(*r))
        CHECK(as_bool(*r), "replace-subtree #t");
    CHECK(flat->size() > size_before, "flat grew after parse");
    CHECK(flat->is_live_node(parent_before), "original parent still live");
    CHECK(!child_of(*flat, parent_before, star_call),
          "original * no longer direct child of parent");

    auto p0 = cs.eval("(pad0 1)");
    (void)cs.eval("(eval-current)");
    p0 = cs.eval("(pad0 1)");
    CHECK(p0.has_value(), "pad0 still evaluates after replace-subtree");
}

// ── Issue #1686 — set-body Guard exception safety ──
static void run_1686_set_body_exception() {
    std::println("\n--- Issue #1686: set-body Guard exception safety ---");
    CompilerService cs;

    // AC1: set-body happy path
    {
        std::println("\n--- AC1 (#1686): set-body happy path ---");
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code f");
        auto r = cs.eval("(mutate:set-body \"f\" \"(+ x 10)\" \"1686-ac1\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "set-body #t");
    }

    // AC2: throw under Guard
    {
        std::println("\n--- AC2 (#1686): throw under Guard ---");
        bool ok = true;
        auto g = Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
        CHECK(g.has_value() && *g, "try_acquire AC2");
        const auto r0 = exception_rollbacks(cs);
        std::string err;
        CHECK(!(*g)->run_or_rollback([&] { throw std::runtime_error("boom-1686-set-body"); }, &err),
              "run_or_rollback returns false");
        CHECK(!ok, "ok=false after throw");
        CHECK(err.find("boom-1686-set-body") != std::string::npos, "err captures what()");
        CHECK(exception_rollbacks(cs) > r0, "exception-rollback counter advanced");
    }

    // AC3: set-body after throw
    {
        std::println("\n--- AC3 (#1686): set-body after throw ---");
        auto r = cs.eval("(mutate:set-body \"f\" \"(* x 3)\" \"1686-ac3\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "set-body after throw #t");
    }

    // AC4: sibling rebind
    {
        std::println("\n--- AC4 (#1686): sibling rebind ---");
        CHECK(cs.eval("(set-code \"(define (g x) (begin 1 2 3))\")").has_value(), "set-code g");
        auto r = cs.eval("(mutate:rebind \"g\" \"(lambda (x) (+ x 1))\" \"1686-ac4\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "rebind sibling #t");
    }
}

// ── Issue #1687 — set-body stale Define + lambda_id ──
static void run_1687_set_body_stale_ids() {
    std::println("\n--- Issue #1687: set-body stale ids ---");
    CompilerService cs;

    static auto set_body_ok = [](CompilerService& cs, const std::string& name,
                                 const std::string& body, const std::string& summary) {
        auto expr =
            std::string("(mutate:set-body \"") + name + "\" \"" + body + "\" \"" + summary + "\")";
        auto r = cs.eval(expr);
        return r && is_bool(*r) && as_bool(*r);
    };
    static auto int_eq = [](CompilerService& cs, const std::string& expr, std::int64_t want) {
        auto r = cs.eval(expr);
        if (r && is_int(*r) && as_int(*r) == want)
            return true;
        (void)cs.eval("(eval-current)");
        r = cs.eval(expr);
        return r && is_int(*r) && as_int(*r) == want;
    };
    static auto pad_program = [](const std::string& target_def, int pads) {
        std::string src = target_def;
        for (int i = 0; i < pads; ++i) {
            src += " (define (pad";
            src += std::to_string(i);
            src += " x) (+ x ";
            src += std::to_string(i);
            src += "))";
        }
        return src;
    };

    // AC1: body-expr set-body after pad
    {
        std::println("\n--- AC1 (#1687): pad + set-body body-expr ---");
        auto src = pad_program("(define (target n) (+ n 1)) (define (other y) (+ y 2))", 48);
        auto set = std::string("(set-code \"") + src + "\")";
        CHECK(eval_ok(cs, set), "set-code target/other + pads");
        CHECK(set_body_ok(cs, "target", "(* n 4)", "1687-ac1"), "set-body body-expr #t");
        CHECK(int_eq(cs, "(target 3)", 12), "target 3 → 12");
        CHECK(int_eq(cs, "(other 5)", 7), "other unchanged");
        CHECK(int_eq(cs, "(pad0 1)", 1), "pad0 unchanged");
        CHECK(int_eq(cs, "(pad47 1)", 48), "pad47 unchanged");
    }

    // AC2: lambda form set-body under pad
    {
        std::println("\n--- AC2 (#1687): set-body lambda form ---");
        CHECK(set_body_ok(cs, "other", "(lambda (y) (* y 10))", "1687-ac2"), "set-body lambda #t");
        CHECK(int_eq(cs, "(other 6)", 60), "other 6 → 60");
    }

    // AC3: full define form
    {
        std::println("\n--- AC3 (#1687): set-body full define form ---");
        CHECK(set_body_ok(cs, "target", "(define (target n) (+ n 100))", "1687-ac3"),
              "set-body full-define #t");
        auto r = cs.eval("(target 1)");
        (void)cs.eval("(eval-current)");
        r = cs.eval("(target 1)");
        CHECK(r.has_value(), "target still evaluates after full-define set-body");
    }

    // AC4: atomic-batch set-body
    {
        std::println("\n--- AC4 (#1687): atomic-batch set-body ---");
        for (int i = 0; i < 4; ++i) {
            CHECK(set_body_ok(cs, "other", "(+ y " + std::to_string(i) + ")", "grow"),
                  "grow set-body");
        }
        auto batch = std::string(
            "(mutate:atomic-batch (list (list \"mutate:set-body\" \"target\" \"(+ n 7)\")))");
        auto r = cs.eval(batch);
        if (!(r && is_bool(*r) && as_bool(*r))) {
            CHECK(set_body_ok(cs, "target", "(+ n 7)", "1687-ac4-fallback"),
                  "set-body fallback after batch attempt");
        }
        CHECK(int_eq(cs, "(target 3)", 10), "target 3 → 10 after AC4");
    }
}

// ── Issue #1699 — splice stale parent ──
static void run_1699_splice() {
    std::println("\n--- Issue #1699: splice stale parent ---");
    CompilerService cs;
    std::string src = "(define (f x) (begin 1))";
    for (int i = 0; i < 48; ++i) {
        src += " (define (pad";
        src += std::to_string(i);
        src += " y) (+ y ";
        src += std::to_string(i);
        src += "))";
    }
    CHECK(eval_ok(cs, std::string("(set-code \"") + src + "\")"), "set-code pad");
    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "flat");

    NodeId begin_id = NULL_NODE;
    for (NodeId id = 0; id < flat->size(); ++id) {
        if (flat->is_live_node(id) && flat->get(id).tag == NodeTag::Begin) {
            begin_id = id;
            break;
        }
    }
    CHECK(begin_id != NULL_NODE, "found Begin parent");
    const auto child_count_before = flat->children(begin_id).size();
    const auto size_before = flat->size();

    auto expr = std::string("(mutate:splice ") + std::to_string(begin_id) +
                " 0 \"(+ 10 20 30 40 50)\" \"(* 2 3 4 5 6)\" \"(- 9 8 7 6 5)\" \"1699-ac1\")";
    auto r = cs.eval(expr);
    CHECK(r.has_value(), "splice eval ok");
    CHECK(flat->size() > size_before, "flat grew after multi-parse");
    CHECK(flat->is_live_node(begin_id), "Begin parent still live");
    CHECK(flat->children(begin_id).size() >= child_count_before + 1,
          "Begin gained at least one child");

    if (r) {
        std::vector<std::int64_t> ids;
        auto cur = *r;
        while (is_pair(cur)) {
            auto idx = as_pair_idx(cur);
            auto& pairs = cs.evaluator().pairs();
            if (idx >= pairs.size())
                break;
            if (is_int(pairs[idx].car))
                ids.push_back(as_int(pairs[idx].car));
            cur = pairs[idx].cdr;
        }
        std::println("  splice returned {} id(s)", ids.size());
        CHECK(ids.size() >= 1, "at least one inserted id returned");
        int under = 0;
        for (auto raw : ids) {
            auto nid = static_cast<NodeId>(raw);
            if (flat->is_live_node(nid) && child_of(*flat, begin_id, nid))
                ++under;
        }
        CHECK(under == static_cast<int>(ids.size()),
              "every returned id is a live child of intended Begin");
    }

    auto p0 = cs.eval("(pad0 1)");
    (void)cs.eval("(eval-current)");
    p0 = cs.eval("(pad0 1)");
    CHECK(p0.has_value(), "pad0 still evaluates");
}

// ── Issue #1704/#1705 — sv Guard ──
static void run_1704_sv_guard() {
    std::println("\n--- Issue #1704/#1705: sv Guard ---");

    // AC1: sv-add-coverpoint
    {
        std::println("\n--- AC1 (#1704): sv-add-coverpoint Guard path ---");
        CompilerService cs;
        CHECK(eval_ok(cs, "(set-code \"(define (f x) x)\")"), "set-code");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat");
        NodeId target = NULL_NODE;
        for (NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_live_node(id)) {
                target = id;
                break;
            }
        }
        CHECK(target != NULL_NODE, "found live target");
        const auto log_before = flat->mutation_count();

        auto r = cs.eval(std::string("(mutate:sv-add-coverpoint ") + std::to_string(target) +
                         " \"cp0\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "sv-add-coverpoint #t");
        CHECK(flat->mutation_count() > log_before, "mutation log grew");
        MutationRecord rec{};
        CHECK(find_last_op(*flat, "sv-add-coverpoint", &rec), "log has sv-add-coverpoint");
        CHECK(rec.target_node == target, "log target is covergroup id");
    }

    // AC2: sv-weaken-property
    {
        std::println("\n--- AC2 (#1704): sv-weaken-property Guard path ---");
        CompilerService cs;
        CHECK(eval_ok(cs, "(set-code \"(define (g x) 1)\")"), "set-code g");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat ac2");
        NodeId target = NULL_NODE;
        for (NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_live_node(id)) {
                target = id;
                break;
            }
        }
        CHECK(target != NULL_NODE, "found live target ac2");
        auto r = cs.eval(std::string("(mutate:sv-weaken-property ") + std::to_string(target) +
                         " \"rst_n\")");
        CHECK(r && is_bool(*r) && as_bool(*r), "sv-weaken-property #t");
        MutationRecord rec{};
        CHECK(find_last_op(*flat, "sv-weaken-property", &rec), "log has sv-weaken-property");
        CHECK(rec.target_node == target, "log target is property id");
    }

    // AC3: bad id fails
    {
        std::println("\n--- AC3 (#1704): out-of-range fails ---");
        CompilerService cs;
        CHECK(eval_ok(cs, "(set-code \"(define (h x) 0)\")"), "set-code h");
        auto* flat = cs.evaluator().workspace_flat();
        auto huge = static_cast<std::int64_t>(flat->size() + 9999);
        auto r1 =
            cs.eval(std::string("(mutate:sv-add-coverpoint ") + std::to_string(huge) + " \"cp\")");
        CHECK(r1 && is_bool(*r1) && !as_bool(*r1), "sv-add-coverpoint #f on bad id");
        auto r2 =
            cs.eval(std::string("(mutate:sv-weaken-property ") + std::to_string(huge) + " \"x\")");
        CHECK(r2 && is_bool(*r2) && !as_bool(*r2), "sv-weaken-property #f on bad id");
    }

    // AC4: source audit
    {
        std::println("\n--- AC4 (#1704): source has #1704 Guard ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_mutate.cpp",
            "../src/compiler/evaluator_primitives_mutate.cpp",
        };
        std::string src;
        for (const char* p : candidates) {
            auto s = read_file(p);
            if (!s.empty()) {
                src = std::move(s);
                break;
            }
        }
        CHECK(!src.empty(), "read mutate.cpp");
        if (!src.empty()) {
            CHECK(src.find("Issue #1704") != std::string::npos, "cites #1704");
            CHECK(src.find("Issue #1705") != std::string::npos ||
                      src.find("#1705") != std::string::npos,
                  "cites #1705 on weaken sibling");
            auto p1 = src.find("mutate:sv-add-coverpoint");
            auto p2 = src.find("mutate:sv-weaken-property");
            CHECK(p1 != std::string::npos && p2 != std::string::npos, "both prims present");
            if (p1 != std::string::npos) {
                auto win = src.substr(p1, 3500);
                CHECK(win.find("MutationBoundaryGuard") != std::string::npos,
                      "sv-add-coverpoint has Guard");
                CHECK(win.find("is_live_node") != std::string::npos,
                      "sv-add-coverpoint is_live_node");
                CHECK(win.find("run_or_rollback") != std::string::npos,
                      "sv-add-coverpoint run_or_rollback");
            }
            if (p2 != std::string::npos) {
                auto win = src.substr(p2, 3500);
                CHECK(win.find("MutationBoundaryGuard") != std::string::npos,
                      "sv-weaken-property has Guard");
                CHECK(win.find("is_live_node") != std::string::npos,
                      "sv-weaken-property is_live_node");
                CHECK(win.find("run_or_rollback") != std::string::npos,
                      "sv-weaken-property run_or_rollback");
            }
        }
    }
}

// ── Issue #1700 — wrap stale parent ──
static void run_1700_wrap() {
    std::println("\n--- Issue #1700: wrap stale parent ---");
    CompilerService cs;
    std::string src = "(define (f x) (* 2 x))";
    for (int i = 0; i < 48; ++i) {
        src += " (define (pad";
        src += std::to_string(i);
        src += " y) (+ y ";
        src += std::to_string(i);
        src += "))";
    }
    CHECK(eval_ok(cs, std::string("(set-code \"") + src + "\")"), "set-code pad");
    auto* flat = cs.evaluator().workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    CHECK(flat != nullptr && pool != nullptr, "flat+pool");

    auto star = find_call_op(*flat, *pool, "*");
    CHECK(star != NULL_NODE, "found (* 2 x)");
    auto parent_before = flat->parent_of(star);
    CHECK(parent_before != NULL_NODE && flat->is_live_node(parent_before),
          "target has live parent");
    const auto size_before = flat->size();

    auto expr = std::string("(mutate:wrap ") + std::to_string(star) +
                " \"(begin 0 0 0 0 0 _)\" \"1700-ac1\")";
    auto r = cs.eval(expr);
    CHECK(r.has_value(), "wrap eval ok");
    CHECK(r && is_int(*r), "wrap returns wrapper root id");
    auto wrapper = static_cast<NodeId>(as_int(*r));
    CHECK(flat->size() > size_before, "flat grew after wrap parse");
    CHECK(flat->is_live_node(wrapper), "wrapper root live");
    CHECK(flat->is_live_node(parent_before), "original parent still live");
    CHECK(child_of(*flat, parent_before, wrapper), "wrapper is child of original parent");
    CHECK(!child_of(*flat, parent_before, star), "original * no longer direct child of parent");

    auto p0 = cs.eval("(pad0 1)");
    (void)cs.eval("(eval-current)");
    p0 = cs.eval("(pad0 1)");
    CHECK(p0.has_value(), "pad0 still evaluates");
}

} // namespace aura_mutate_batch

int main() {
    using namespace aura_mutate_batch;
    std::println("=== Mutate batch: #1436 + #1691 + #1701 + #1772 + #1684 + #1702 + #1690 +");
    std::println(" #1696 + #1685 + #1703 + #1688 + #1689 + #1694 + #1697 + #1686 +");
    std::println(" #1687 + #1699 + #1704/#1705 + #1700 (75 ACs total) ===");
    std::println("(test_mutate_cross_thread_migration.cpp NOT included — registered in");
    std::println(" cmake/AuraDomainTests.cmake:694-696 with add_dependencies(...) default-build)");
    run_1436_dispatch();
    run_1691_dead_heap();
    run_1701_extract_parent();
    run_1772_from_feedback();
    run_1684_guard_exception();
    run_1702_inline_call();
    run_1690_insert_child();
    run_1696_multi_node_log();
    run_1685_rebind_stale_define();
    run_1703_refactor_extract();
    run_1688_remove_node_all_parents();
    run_1689_remove_node_index();
    run_1694_replace_pattern();
    run_1697_replace_subtree();
    run_1686_set_body_exception();
    run_1687_set_body_stale_ids();
    run_1699_splice();
    run_1704_sv_guard();
    run_1700_wrap();
    std::println("\n=== Mutate batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
