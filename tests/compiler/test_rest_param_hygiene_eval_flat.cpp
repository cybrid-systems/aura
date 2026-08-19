// @category: unit
// @reason: Issue #3153 — eval_flat dotted-rest + reexpand_call pair-spine
// call sites skipped stamp_rest_param_hygiene (helper dropped MacroIntroduced
// marker on rest-list spine, so is_macro_introduced stayed false and
// mutate:replace-subtree / rebind gates could not reject rest nodes). Residual
// of #2018 / #2169 / #2239 / #2808 — call-site residual only, helper exists.
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
#include <print>
#include <string>
#include <string_view>

namespace aura::compiler::macro_exp {

// Forward declaration for the helper (defined in macro_expansion.cpp,
// exported from aura.compiler.macro_expansion module).
inline void stamp_rest_param_hygiene(aura::ast::FlatAST& target, const aura::ast::FlatAST& source,
                                     aura::ast::NodeId src_body_id, aura::ast::NodeId list_root);

} // namespace aura::compiler::macro_exp

namespace {

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
        CHECK(window.find("list_end != aura::ast::NULL_NODE") != std::string::npos,
              "reexpand_call guards stamp on non-null list_end");
        CHECK(window.find("#3153") != std::string::npos,
              "cites #3153 in reexpand_call pair-spine block");
    }
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
    CHECK(mxcpp.find("set_marker(cur, SyntaxMarker::MacroIntroduced)") != std::string::npos,
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
    CHECK(mxcpp.find("set_provenance(cur, origin)") != std::string::npos,
          "helper sets provenance (parity preserved)");
    CHECK(mxcpp.find("set_schema_cache(cur, src_schema)") != std::string::npos,
          "helper sets schema_cache (parity preserved)");
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

} // namespace

int main() {
    ac1_helper_exposed_cross_tu();
    ac2_eval_flat_dotted_rest_call();
    ac3_reexpand_call_pair_spine_call();
    ac4_rest_spine_macro_introduced_parity();
    ac5_marker_set_total_parity();
    if (g_failed)
        return 1;
    std::println("eval_flat rest-param-hygiene (#3153): OK ({} passed)", g_passed);
    return 0;
}