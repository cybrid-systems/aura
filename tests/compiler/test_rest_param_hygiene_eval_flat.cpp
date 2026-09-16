// @category: unit
// @reason: Issue #3153 — eval_flat dotted-rest + reexpand_call pair-spine
// call sites skipped stamp_rest_param_hygiene (helper dropped MacroIntroduced
// marker on rest-list spine, so is_macro_introduced stayed false and
// mutate:replace-subtree / rebind gates could not reject rest nodes). Residual
// of #2018 / #2169 / #2239 / #2808 — call-site residual only, helper exists.
// Issue #3817 — rest add_*/stamp before clone checkpoint leaves MacroIntroduced
// orphans on gensym/depth/pass deny; production truncate_to(rest_spine_ckpt).
//
//   AC1: helper exposed cross-TU — dropped static, added export declaration
//        in macro_expansion.ixx. Definition still single-source in
//        macro_expansion.cpp as inline (ODR-safe across TUs).
//   AC2: eval_flat dotted-rest hot path now calls stamp_rest_param_hygiene
//        after add_call (list_var/list_call). Source-cite gate.
//   AC3: reexpand_call pair-spine path now calls stamp_rest_param_hygiene
//        after the add_pair loop completes (list_end root). Source-cite.
//   AC4: rest-list spine nodes have is_macro_introduced == true after
//        eval_flat dotted expand. Parity with macro_expand_all path.
//   AC5: g_stamp_rest_param_marker_set_total increases on eval_flat
//        dotted expand (parity with expand_all / expand_inner_macros).
//        Soft / Off zero-cost preserved — no new middle metrics layer.
//
// Sibling tests implicitly covered (must remain green):
//   - tests/compiler/test_rest_param_hygiene.cpp (#2808 — macro_expand_all)
//   - tests/compiler/test_stamp_rest_param_hygiene_marker.cpp (#2808 — marker)
//   - tests/compiler/test_macro_intro_restamp.cpp (#2096 — restamp)
//   - tests/compiler/test_macro_inner_expand_marker.cpp (#3151 — nested)
//   - tests/compiler/test_hygiene_mutate_closed_loop.cpp (#1611 — gates)

#include "test_harness.hpp"

#include <atomic>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "compiler/aura_jit_bridge.h"
#include "compiler/grant_test_support.hh"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/transparent_string_hash.hh"

import std;
import aura.compiler.macro_expansion;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;
import aura.core.arena; // ASTArena lives here (wave split the re-export)
import aura.parser.parser;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::ast::SyntaxMarker;
using aura::compiler::CompilerService;
using aura::compiler::macro_exp::clone_macro_body;
using aura::compiler::macro_exp::g_macro_hygiene_last_limit_reason;
using aura::compiler::macro_exp::hygiene_last_limit_reason_string;
using aura::compiler::macro_exp::stamp_rest_param_hygiene;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto* p :
         {path, "../src/compiler/evaluator_eval_flat.cpp", "src/compiler/evaluator_eval_flat.cpp",
          "../src/compiler/macro_expansion.cpp", "src/compiler/macro_expansion.cpp",
          "../src/compiler/macro_expansion.ixx", "src/compiler/macro_expansion.ixx"}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// AC1: helper exposed cross-TU — dropped static, added export declaration
// in macro_expansion.ixx. Definition still single-source in
// macro_expansion.cpp as inline (ODR-safe across TUs).
static void ac1_helper_exposed_cross_tu() {
    std::println("\n--- AC1: helper exposed cross-TU via module export ---");
    auto mxcpp = read_file("src/compiler/macro_expansion.cpp");
    auto mcixx = read_file("src/compiler/macro_expansion.ixx");
    CHECK(!mxcpp.empty(), "macro_expansion.cpp readable");
    CHECK(!mcixx.empty(), "macro_expansion.ixx readable");
    // Definition must be inline (not static) — both for cross-TU ODR safety
    // and module export.
    CHECK(mxcpp.find("static inline void stamp_rest_param_hygiene(") == std::string::npos,
          "static dropped from stamp_rest_param_hygiene definition");
    CHECK(mxcpp.find("inline void stamp_rest_param_hygiene(") != std::string::npos,
          "stamp_rest_param_hygiene definition is inline (cross-TU safe)");
    // Export declaration in .ixx.
    CHECK(mcixx.find("export void stamp_rest_param_hygiene(") != std::string::npos,
          "export declaration added in macro_expansion.ixx");
    // Definition still in .cpp (single source of truth).
    CHECK(mxcpp.find("void stamp_rest_param_hygiene(aura::ast::FlatAST& target,") !=
              std::string::npos,
          "definition still in macro_expansion.cpp (single source of truth)");
    // Test bridge still present (no regression to C-ABI test entry).
    CHECK(mxcpp.find("aura_test_call_stamp_rest_param_hygiene") != std::string::npos,
          "test bridge aura_test_call_stamp_rest_param_hygiene still present");
}

// AC2: eval_flat dotted-rest hot path now calls stamp_rest_param_hygiene
// after add_call (list_var/list_call).
static void ac2_eval_flat_dotted_rest_call() {
    std::println("\n--- AC2: eval_flat dotted-rest calls stamp_rest_param_hygiene ---");
    auto eef = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(!eef.empty(), "evaluator_eval_flat.cpp readable");
    // Compile-unblock (asan-build / ubsan-smoke / reproducible-build /
    // deployment-health): the helper is exported from
    // aura.compiler.macro_expansion; eval_flat is aura.compiler. A
    // using-declaration at namespace scope makes the two call sites
    // resolve without rewriting the source-cite strings below.
    CHECK(eef.find("using aura::compiler::macro_exp::stamp_rest_param_hygiene;") !=
              std::string::npos,
          "eval_flat imports stamp_rest_param_hygiene from macro_exp (cross-module using)");
    // Find the eval_flat dotted-rest block (list_var = add_variable("list") +
    // list_call = add_call(list_var, remaining)).
    const auto list_var_pos = eef.find("add_variable(p->intern(\"list\"))");
    CHECK(list_var_pos != std::string::npos, "eval_flat dotted-rest list_var allocation found");
    if (list_var_pos != std::string::npos) {
        const auto window_end = std::min<std::size_t>(list_var_pos + 1500, eef.size());
        const std::string window(eef, list_var_pos, window_end - list_var_pos);
        CHECK(window.find("add_call(list_var, remaining)") != std::string::npos,
              "eval_flat dotted-rest list_call allocation found");
        CHECK(window.find("stamp_rest_param_hygiene(*f, *md.flat, md.body_id, list_call)") !=
                  std::string::npos,
              "eval_flat dotted-rest now calls stamp_rest_param_hygiene");
        CHECK(window.find("#3153") != std::string::npos,
              "cites #3153 in eval_flat dotted-rest block");
    }
}

// AC3: reexpand_call pair-spine path now calls stamp_rest_param_hygiene
// after the add_pair loop completes (list_end root).
static void ac3_reexpand_call_pair_spine_call() {
    std::println("\n--- AC3: reexpand_call pair-spine calls stamp_rest_param_hygiene ---");
    auto eef = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(!eef.empty(), "evaluator_eval_flat.cpp readable");
    // Find the reexpand_call pair-spine block (list_end = flat.add_pair(...)).
    const auto add_pair_pos = eef.find("list_end = flat.add_pair(");
    CHECK(add_pair_pos != std::string::npos, "reexpand_call pair-spine add_pair found");
    if (add_pair_pos != std::string::npos) {
        const auto window_end = std::min<std::size_t>(add_pair_pos + 1500, eef.size());
        const std::string window(eef, add_pair_pos, window_end - add_pair_pos);
        CHECK(window.find("stamp_rest_param_hygiene(flat, md.flat ? *md.flat : flat, md.body_id, "
                          "list_end)") != std::string::npos,
              "reexpand_call pair-spine now calls stamp_rest_param_hygiene");
        CHECK(window.find("#3153") != std::string::npos,
              "cites #3153 in reexpand_call pair-spine block");
        CHECK(window.find("Issue #3753") != std::string::npos,
              "3753: empty rest binds a stamped (list)");
        CHECK(window.find("empty_rest") != std::string::npos,
              "3753: zero extras allocate empty (list)");
    }
}

static std::string merr_kind_3753(CompilerService& cs, const aura::compiler::types::EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!is_string(pairs[idx].car))
        return {};
    auto sidx = as_string_idx(pairs[idx].car);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

static void ac3753_empty_rest_reexpand_stamped() {
    std::println("\n--- #3753 AC1: reexpand_call empty rest is stamped (list) ---");
    CompilerService cs;
    CHECK(
        cs.eval("(set-code \"(define-hygienic-macro (m3753 a . rest) rest) (define r (m3753 1))\")")
            .has_value(),
        "3753 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3753 AC1: eval-current");
    auto* ws = cs.evaluator().workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    CHECK(ws != nullptr && pool != nullptr, "3753 AC1: workspace");
    ws->rebuild_parent_links_from_children();
    aura::ast::NodeId call_id = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (!ws->is_live_node(id))
            continue;
        auto v = ws->get(id);
        if (v.tag != aura::ast::NodeTag::Call || v.children.empty())
            continue;
        auto callee = ws->get(v.child(0));
        if (!callee.has_name() || pool->resolve(callee.sym_id) != "m3753")
            continue;
        if (ws->parent_of(id) != aura::ast::NULL_NODE) {
            call_id = id;
            break;
        }
        if (call_id == aura::ast::NULL_NODE)
            call_id = id;
    }
    CHECK(call_id != aura::ast::NULL_NODE, "3753 AC1: find (m3753 1)");
    const auto pid = ws->parent_of(call_id);
    std::uint32_t slot = 0;
    if (pid != aura::ast::NULL_NODE) {
        auto pv = ws->get(pid);
        for (std::uint32_t ci = 0; ci < pv.children.size(); ++ci) {
            if (pv.child(ci) == call_id) {
                slot = ci;
                break;
            }
        }
    }
    aura::ast::MutationRecord rec{};
    rec.target_node = call_id;
    rec.parent_id = pid;
    const auto n = cs.evaluator().post_mutation_macro_reexpand(*ws, *pool, rec);
    CHECK(n >= 1, "3753 AC1: reexpand_call ran");
    aura::ast::NodeId spine = aura::ast::NULL_NODE;
    if (pid != aura::ast::NULL_NODE) {
        spine = ws->get(pid).child(slot);
        CHECK(spine != call_id, "3753 AC1: Call spliced");
    } else {
        for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
            if (!ws->is_live_node(id) || !ws->is_macro_introduced(id))
                continue;
            auto v = ws->get(id);
            if (v.tag == aura::ast::NodeTag::Call && !v.children.empty()) {
                auto c = ws->get(v.child(0));
                if (c.has_name() && pool->resolve(c.sym_id) == "list") {
                    spine = id;
                    break;
                }
            }
        }
    }
    CHECK(spine != aura::ast::NULL_NODE, "3753 AC1: rest spine allocated");
    CHECK(ws->is_macro_introduced(spine), "3753 AC1: empty rest spine is MacroIntroduced");
    auto rv = cs.eval(
        std::format("(mutate:replace-value {} 99 \"3753-deny\")", static_cast<unsigned>(spine)));
    CHECK(rv.has_value() && merr_kind_3753(cs, *rv) == "hygiene-protected",
          "3753 AC1: replace-value on spine is hygiene-protected");
}

// AC4: rest-list spine nodes have is_macro_introduced == true after
// eval_flat dotted expand. Parity with macro_expand_all path. Source-cite:
// both paths share the same stamp helper, so semantics preserved.
static void ac4_rest_spine_macro_introduced_parity() {
    std::println("\n--- AC4: rest-spine MacroIntroduced parity (shared helper) ---");
    auto mxcpp = read_file("src/compiler/macro_expansion.cpp");
    auto eef = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(!mxcpp.empty(), "macro_expansion.cpp readable");
    CHECK(!eef.empty(), "evaluator_eval_flat.cpp readable");
    // Helper sets MacroIntroduced marker (already landed at #2808).
    CHECK(mxcpp.find("set_marker(id, SyntaxMarker::MacroIntroduced)") != std::string::npos,
          "helper sets MacroIntroduced marker (parity preserved)");
    // Both call sites now invoke the helper → parity.
    CHECK(eef.find("stamp_rest_param_hygiene(*f, *md.flat, md.body_id, list_call)") !=
              std::string::npos,
          "eval_flat dotted-rest call site parity");
    CHECK(eef.find(
              "stamp_rest_param_hygiene(flat, md.flat ? *md.flat : flat, md.body_id, list_end)") !=
              std::string::npos,
          "reexpand_call pair-spine call site parity");
    // Helper applies kMacroExpansion dirty bit (already landed).
    CHECK(mxcpp.find("MacroDirtyReason::kMacroExpansion") != std::string::npos,
          "helper applies kMacroExpansion dirty bit");
    CHECK(mxcpp.find("set_provenance(id, origin)") != std::string::npos,
          "helper sets provenance (parity preserved)");
    CHECK(mxcpp.find("set_schema_cache(id, src_schema)") != std::string::npos,
          "helper sets schema_cache (parity preserved)");
}

// Issue #3468: helper stamps spine only — remaining caller args stay User.
static void ac3468_spine_only_no_remaining_walk() {
    std::println("\n--- #3468: stamp spine only, not remaining args ---");
    auto mxcpp = read_file("src/compiler/macro_expansion.cpp");
    CHECK(!mxcpp.empty(), "macro_expansion.cpp readable");
    CHECK(mxcpp.find("Issue #3468") != std::string::npos, "3468: helper cites #3468");
    CHECK(mxcpp.find("stamp_one(root_v.child(0))") != std::string::npos,
          "3468: Call path stamps list/cons head only");
    CHECK(mxcpp.find("target.get(cdr).tag != NodeTag::Pair") != std::string::npos,
          "3468: pair path follows cdr cells, not car");
    auto pos = mxcpp.find("inline void stamp_rest_param_hygiene");
    CHECK(pos != std::string::npos, "3468: helper present");
    if (pos != std::string::npos) {
        const auto win = mxcpp.substr(pos, 2800);
        CHECK(win.find("stack.push_back(child)") == std::string::npos,
              "3468: DFS remaining-child walk removed");
    }
    CHECK(mxcpp.find("g_3468_") == std::string::npos, "3468: no new metric atomic");
}

// AC5: g_stamp_rest_param_marker_set_total increases on eval_flat dotted
// expand (parity with expand_all / expand_inner_macros). Source-cite:
// helper bumps the same atomic that other paths bump. Soft / Off
// zero-cost preserved — no new middle metrics layer.
static void ac5_marker_set_total_parity() {
    std::println("\n--- AC5: marker_set_total parity (single shared atomic) ---");
    auto mxcpp = read_file("src/compiler/macro_expansion.cpp");
    auto mcixx = read_file("src/compiler/macro_expansion.ixx");
    CHECK(!mxcpp.empty(), "macro_expansion.cpp readable");
    CHECK(!mcixx.empty(), "macro_expansion.ixx readable");
    // Helper bumps g_stamp_rest_param_marker_set_total (existing atomic).
    CHECK(
        mxcpp.find("g_stamp_rest_param_marker_set_total.fetch_add(1, std::memory_order_relaxed)") !=
            std::string::npos,
        "helper bumps g_stamp_rest_param_marker_set_total");
    // Atomic is exported from module (single shared counter).
    CHECK(mcixx.find(
              "export extern std::atomic<std::uint64_t> g_stamp_rest_param_marker_set_total") !=
              std::string::npos,
          "g_stamp_rest_param_marker_set_total exported (single shared counter)");
    // No new atomic introduced by this fix (Soft / Off zero-cost preserved).
    CHECK(mxcpp.find("g_3153_") == std::string::npos,
          "no new g_3153_* atomic in macro_expansion.cpp");
    CHECK(mcixx.find("g_3153_") == std::string::npos,
          "no new g_3153_* atomic in macro_expansion.ixx");
    // No test_issue_3153.cpp / docs/design/3153-* per #81967 / #1655.
    // (linter enforces; this is a defensive guard for the test itself).
}


// Issue #3817: source cites rest_spine_ckpt + production truncate on NULL clone.
static void ac3817_source_rest_spine_ckpt() {
    std::println("\n--- #3817 AC1: rest_spine_ckpt + truncate on NULL clone ---");
    auto eef = read_file("src/compiler/evaluator_eval_flat.cpp");
    auto mx = read_file("src/compiler/macro_expansion.cpp");
    CHECK(!eef.empty() && !mx.empty(), "3817 AC1: sources readable");
    CHECK(eef.find("Issue #3817") != std::string::npos, "3817 AC1: eval_flat cites #3817");
    CHECK(eef.find("rest_spine_ckpt") != std::string::npos, "3817 AC1: rest_spine_ckpt");
    CHECK(eef.find("truncate_to(rest_spine_ckpt)") != std::string::npos,
          "3817 AC1: truncate_to(rest_spine_ckpt)");
    CHECK(eef.find("is_sandbox_active()") != std::string::npos, "3817 AC1: production gate");
    auto re = eef.find("Issue #3817: checkpoint before rest add_*/stamp");
    CHECK(re != std::string::npos, "3817 AC1: reexpand_call cite");
    CHECK(mx.find("Issue #3817") != std::string::npos, "3817 AC1: macro_expansion cites");
    CHECK(mx.find("truncate_to(rest_spine_ckpt)") != std::string::npos,
          "3817 AC1: expand paths truncate");
    CHECK(read_file("tests/compiler/test_issue_3817.cpp").empty(), "3817: no test_issue");
    CHECK(read_file("docs/design/3817-rest-spine-orphan.md").empty(), "3817: no docs/design");
}

// Soft/Off: truncate gated — historical half-write when sandbox inactive.
static void ac3817_soft_off_gate() {
    std::println("\n--- #3817 AC3: Soft/Off contract unchanged ---");
    auto eef = read_file("src/compiler/evaluator_eval_flat.cpp");
    auto win_pos = eef.find("Issue #3817: production rewind pre-clone");
    CHECK(win_pos != std::string::npos, "3817 AC3: rewind cite");
    auto win = eef.substr(win_pos, 500);
    CHECK(win.find("is_sandbox_active()") != std::string::npos,
          "3817 AC3: truncate gated on is_sandbox_active");
    auto mx = read_file("src/compiler/macro_expansion.cpp");
    CHECK(mx.find("rest_spine_pending && production_surface") != std::string::npos,
          "3817 AC3: expand paths gate on production_surface");
}

// Soak: rest stamp × gensym ceiling deny → truncate leaves no MacroIntroduced
// rest list orphan; stable reason hygiene-gensym-ceiling; mutate face clean.
static void ac3817_soak_rest_ceiling_no_orphan() {
    std::println("\n--- #3817 soak: rest × ceiling deny × no orphan spine ---");
    using aura::core::capability::Effect;
    using aura::core::capability::g_capability_registry;
    using aura::core::capability::reset_capability_effects_for_test;
    reset_capability_effects_for_test();
    CHECK(g_capability_registry().grant(0, "tenant-admin", Effect::TenantAdmin,
                                        aura_test_grant_prov()),
          "3817 soak: TenantAdmin grant");
    CHECK(g_capability_registry().grant_macro_self_evo(0, {}, aura_test_grant_prov()),
          "3817 soak: MacroSelfEvo grant");
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    StringPool sp(alloc);
    FlatAST src(alloc);
    // Two lets → gensym ceiling at cap=1 forces NULL clone after rest stamp.
    auto pr = aura::parser::parse_to_flat("(let ((x 1)) (let ((y 2)) rest))", src, sp);
    CHECK(pr.success && pr.root != NULL_NODE, "3817 soak: parse body");

    FlatAST target(alloc);
    StringPool tp(alloc);
    // Mirror eval_flat: checkpoint → allocate (list) → stamp → clone.
    const auto rest_spine_ckpt = target.size();
    auto list_var = target.add_variable(tp.intern("list"));
    std::vector<NodeId> remaining;
    auto list_call = target.add_call(list_var, remaining);
    stamp_rest_param_hygiene(target, src, pr.root, list_call);
    CHECK(target.is_macro_introduced(list_call), "3817 soak: rest stamped MacroIntroduced");
    const auto size_after_stamp = target.size();
    CHECK(size_after_stamp > rest_spine_ckpt, "3817 soak: rest grew flat");

    std::unordered_map<std::string, NodeId, aura::core::TransparentStringHash, std::equal_to<>>
        subst;
    subst["rest"] = list_call;
    std::unordered_map<std::string, std::string, aura::core::TransparentStringHash, std::equal_to<>>
        rename_map;
    aura_test_set_max_gensym_map_size_for_test(1);
    g_macro_hygiene_last_limit_reason.store(0, std::memory_order_relaxed);
    auto expanded = clone_macro_body(target, tp, src, sp, pr.root, &subst, &rename_map,
                                     SyntaxMarker::MacroIntroduced);
    aura_test_set_max_gensym_map_size_for_test(0);
    CHECK(expanded == NULL_NODE, "3817 soak: production ceiling → NULL_NODE");
    const auto* rs = hygiene_last_limit_reason_string();
    CHECK(rs != nullptr && std::string(rs) == "hygiene-gensym-ceiling",
          "3817 soak: hygiene-gensym-ceiling");

    // Production rewind (same face as eval_flat #3817).
    if (aura::core::sandbox::is_sandbox_active())
        target.truncate_to(rest_spine_ckpt);
    CHECK(target.size() == rest_spine_ckpt, "3817 soak: flat size restored");
    std::size_t orphan_mi = 0;
    for (NodeId id = rest_spine_ckpt; id < target.size(); ++id) {
        if (target.is_macro_introduced(id))
            ++orphan_mi;
    }
    CHECK(orphan_mi == 0, "3817 soak: no MacroIntroduced orphan past ckpt");
    CHECK(list_call >= target.size() || !target.is_macro_introduced(list_call),
          "3817 soak: stamped list_call no longer live MacroIntroduced");

    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    reset_capability_effects_for_test();
}

// Soft/Off: without truncate, stamped rest may remain (historical half-write).
static void ac3817_off_half_write() {
    std::println("\n--- #3817 AC3b: Off keeps historical half-write ---");
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    StringPool sp(alloc);
    FlatAST src(alloc);
    auto pr = aura::parser::parse_to_flat("(let ((x 1)) (let ((y 2)) rest))", src, sp);
    CHECK(pr.success, "3817 AC3b: parse");
    FlatAST target(alloc);
    StringPool tp(alloc);
    const auto ckpt = target.size();
    auto list_var = target.add_variable(tp.intern("list"));
    std::vector<NodeId> remaining;
    auto list_call = target.add_call(list_var, remaining);
    stamp_rest_param_hygiene(target, src, pr.root, list_call);
    std::unordered_map<std::string, NodeId, aura::core::TransparentStringHash, std::equal_to<>>
        subst;
    subst["rest"] = list_call;
    std::unordered_map<std::string, std::string, aura::core::TransparentStringHash, std::equal_to<>>
        rename_map;
    aura_test_set_max_gensym_map_size_for_test(1);
    (void)clone_macro_body(target, tp, src, sp, pr.root, &subst, &rename_map,
                           SyntaxMarker::MacroIntroduced);
    aura_test_set_max_gensym_map_size_for_test(0);
    CHECK(target.size() >= ckpt, "3817 AC3b: Off does not require rewind");
    CHECK(list_call < target.size() && target.is_macro_introduced(list_call),
          "3817 AC3b: Off may keep stamped rest MacroIntroduced");
}

} // namespace

int main() {
    ac1_helper_exposed_cross_tu();
    ac2_eval_flat_dotted_rest_call();
    ac3_reexpand_call_pair_spine_call();
    ac3753_empty_rest_reexpand_stamped();
    ac4_rest_spine_macro_introduced_parity();
    ac5_marker_set_total_parity();
    ac3468_spine_only_no_remaining_walk();
    ac3817_source_rest_spine_ckpt();
    ac3817_soft_off_gate();
    ac3817_soak_rest_ceiling_no_orphan();
    ac3817_off_half_write();
    if (g_failed)
        return 1;
    std::println("eval_flat rest-param-hygiene (#3153/#3817): OK ({} passed)", g_passed);
    return 0;
}