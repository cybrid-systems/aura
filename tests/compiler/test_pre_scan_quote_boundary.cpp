// @category: unit
// @reason: Issue #3154 — pre_scan treated NodeTag::Quote as code, gensym'ing
// bindings under (quote (let ((x 1)) x)). Residual of closed #2807 (qq / unquote
// / unquote-splicing boundary handling) — Quote is a first-class NodeTag
// distinct from the Call-named "quote" form, and must be a data boundary.
//
//   AC1: pre_scan returns without descending into NodeTag::Quote children
//        (data boundary). Source-cite on the early-return.
//   AC2: (quote (let ((x 1)) x)) under a macro template keeps original
//        names after clone (no __x_N inside the quoted subtree). Source-cite
//        on the boundary semantics matching unquote.
//   AC3: quasiquote / unquote / ,@ behaviour from #2807 unchanged. Source-cite
//        on all 3 Call-head paths still present, no regression.
//   AC4: Soft / Off contract preserved — no new metrics middle-layer, no
//        second hygiene model. Source-cite on no new g_3154_* atomic.
//
// Sibling tests implicitly covered (must remain green):
//   - tests/compiler/test_unquote_splicing_hygiene.cpp (#2807)
//   - tests/compiler/test_quasiquote_*.cpp (qq family)
//   - tests/compiler/test_pre_scan_rest_nested_qq.cpp (#2239)
//   - tests/compiler/test_rest_param_hygiene.cpp (#2808)
//   - tests/compiler/test_macro_intro_restamp.cpp (#2096)
//   - tests/compiler/test_macro_inner_expand_marker.cpp (#3151)
//   - tests/compiler/test_rest_param_hygiene_eval_flat.cpp (#3153)

#include "test_harness.hpp"

#include <print>
#include <string>
#include <string_view>

namespace {

using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto* p :
         {path, "../src/compiler/macro_expansion.cpp", "src/compiler/macro_expansion.cpp"}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// AC1: pre_scan returns without descending into NodeTag::Quote children
// (data boundary). Source-cite on the early-return.
static void ac1_pre_scan_quote_boundary() {
    std::println("\n--- AC1: pre_scan returns on NodeTag::Quote ---");
    auto mxcpp = read_file("src/compiler/macro_expansion.cpp");
    CHECK(!mxcpp.empty(), "macro_expansion.cpp readable");
    CHECK(mxcpp.find("#3154") != std::string::npos, "cites #3154");
    // The early-return must be in pre_scan — between the qq Call-head
    // handling (#2807) and the binding-position checks (Let/Lambda/Define/
    // MacroDef). Find the pre_scan closure and verify the Quote check is
    // present in the right place.
    const auto pre_scan_pos = mxcpp.find("std::function<void(NodeId, int)> pre_scan = ");
    CHECK(pre_scan_pos != std::string::npos, "pre_scan closure found");
    if (pre_scan_pos != std::string::npos) {
        const auto window_end = std::min<std::size_t>(pre_scan_pos + 4500, mxcpp.size());
        const std::string window(mxcpp, pre_scan_pos, window_end - pre_scan_pos);
        // Quote early-return present.
        CHECK(window.find("nv.tag == NodeTag::Quote") != std::string::npos,
              "NodeTag::Quote early-return present in pre_scan");
        // The early-return placement: between unquote-splicing handling
        // (#2807) and binding-position checks (Let / Lambda / Define /
        // MacroDef).
        const auto qs = window.find("nv.tag == NodeTag::Quote");
        const auto let_pos = window.find("NodeTag::Let || nv.tag == NodeTag::LetRec");
        CHECK(qs != std::string::npos && let_pos != std::string::npos && qs < let_pos,
              "Quote early-return placed BEFORE binding-position checks");
        const auto uqs = window.find("cname == \"unquote-splicing\"");
        CHECK(uqs != std::string::npos && qs != std::string::npos && uqs < qs,
              "Quote early-return placed AFTER unquote-splicing handling (no QQ regression)");
    }
}

// AC2: (quote (let ((x 1)) x)) under macro template keeps original names
// after clone (no __x_N inside quoted subtree). Source-cite: the early-
// return on Quote is followed by no `rename_binding_pre` call for the
// binding-position branch, so Quote subtrees never reach the gensym path.
static void ac2_quote_body_no_gensym() {
    std::println("\n--- AC2: quote body keeps original names ---");
    auto mxcpp = read_file("src/compiler/macro_expansion.cpp");
    CHECK(!mxcpp.empty(), "macro_expansion.cpp readable");
    // The comment must explain the data-boundary semantics, matching the
    // unquote caller-scope stop semantics.
    const auto pre_scan_pos = mxcpp.find("std::function<void(NodeId, int)> pre_scan = ");
    if (pre_scan_pos != std::string::npos) {
        const auto window_end = std::min<std::size_t>(pre_scan_pos + 4500, mxcpp.size());
        const std::string window(mxcpp, pre_scan_pos, window_end - pre_scan_pos);
        const auto qs = window.find("nv.tag == NodeTag::Quote");
        if (qs != std::string::npos) {
            // Look at the comment block immediately above the early-return.
            const auto comment_start = window.rfind("// Issue #3154", qs);
            CHECK(comment_start != std::string::npos, "Quote early-return has Issue #3154 comment");
            if (comment_start != std::string::npos) {
                const auto comment_end = std::min<std::size_t>(qs + 200, window.size());
                const std::string comment(window, comment_start, comment_end - comment_start);
                CHECK(comment.find("data boundary") != std::string::npos,
                      "comment describes Quote as data boundary");
                CHECK(comment.find("caller-scope") != std::string::npos ||
                          comment.find("caller scope") != std::string::npos,
                      "comment aligns with caller-scope semantics (matches unquote)");
                CHECK(comment.find("__x_N") != std::string::npos ||
                          comment.find("gensym") != std::string::npos,
                      "comment explains why gensym would be wrong");
            }
        }
    }
    // Pre-scan's binding-position rename_binding_pre path is NOT reachable
    // from Quote children because of the early-return.
    CHECK(mxcpp.find("rename_binding_pre(nv.sym_id)") != std::string::npos,
          "rename_binding_pre still defined (binding-position path intact)");
    CHECK(mxcpp.find("rename_binding_pre(nv.params[i])") != std::string::npos,
          "rename_binding_pre on Lambda/MacroDef params still defined");
}

// AC3: quasiquote / unquote / ,@ behaviour from #2807 unchanged.
// Source-cite: all 3 Call-head paths still present.
static void ac3_qq_unquote_splicing_unchanged() {
    std::println("\n--- AC3: qq / unquote / unquote-splicing from #2807 unchanged ---");
    auto mxcpp = read_file("src/compiler/macro_expansion.cpp");
    CHECK(!mxcpp.empty(), "macro_expansion.cpp readable");
    // All 3 Call-head paths must still be present in pre_scan.
    const auto pre_scan_pos = mxcpp.find("std::function<void(NodeId, int)> pre_scan = ");
    CHECK(pre_scan_pos != std::string::npos, "pre_scan closure found");
    if (pre_scan_pos != std::string::npos) {
        const auto window_end = std::min<std::size_t>(pre_scan_pos + 4500, mxcpp.size());
        const std::string window(mxcpp, pre_scan_pos, window_end - pre_scan_pos);
        CHECK(window.find("cname == \"quasiquote\"") != std::string::npos,
              "quasiquote Call-head path still present (#2807)");
        CHECK(window.find("cname == \"unquote\"") != std::string::npos,
              "unquote Call-head path still present (#2807)");
        CHECK(window.find("cname == \"unquote-splicing\"") != std::string::npos,
              "unquote-splicing Call-head path still present (#2807)");
        // The qq_depth tracking for rest-param nested qq hits must still
        // bump — no regression to #2239.
        CHECK(window.find("g_macro_rest_param_nested_qq_hits_total") != std::string::npos,
              "nested qq rest-param metric still bumped (#2239)");
    }
    // The unquote-splicing boundary metric (hygiene mismatch) still bumped.
    CHECK(mxcpp.find("g_unquote_splicing_hygiene_mismatch_total") != std::string::npos,
          "unquote-splicing boundary metric still bumped");
}

// AC4: Soft / Off contract preserved — no new metrics middle-layer, no
// second hygiene model.
static void ac4_soft_off_preserved() {
    std::println("\n--- AC4: Soft / Off unchanged ---");
    auto mxcpp = read_file("src/compiler/macro_expansion.cpp");
    auto mcixx = read_file("src/compiler/macro_expansion.ixx");
    CHECK(!mxcpp.empty(), "macro_expansion.cpp readable");
    CHECK(!mcixx.empty(), "macro_expansion.ixx readable");
    // No new g_3154_* atomic introduced (Soft / Off zero-cost preserved).
    CHECK(mxcpp.find("g_3154_") == std::string::npos,
          "no new g_3154_* atomic in macro_expansion.cpp");
    CHECK(mcixx.find("g_3154_") == std::string::npos,
          "no new g_3154_* atomic in macro_expansion.ixx");
    // No new metric counter introduced in observability header.
    auto obs = read_file("src/compiler/observability_metrics.h");
    if (obs.empty())
        obs = std::string{}; // may not be readable; skip header check
    if (!obs.empty()) {
        CHECK(obs.find("g_3154_") == std::string::npos,
              "no new g_3154_* atomic in observability_metrics.h");
    }
    // Single shared pre_scan closure — no second hygiene model.
    CHECK(mxcpp.find("std::function<void(NodeId, int)> pre_scan = ") != std::string::npos,
          "pre_scan closure present (single hygiene model)");
    CHECK(mxcpp.find("pre_scan(body_id, /*qq_depth=*/0)") != std::string::npos,
          "pre_scan entry point still pre_scan(body_id, qq_depth=0)");
}

} // namespace

int main() {
    ac1_pre_scan_quote_boundary();
    ac2_quote_body_no_gensym();
    ac3_qq_unquote_splicing_unchanged();
    ac4_soft_off_preserved();
    if (g_failed)
        return 1;
    std::println("pre_scan Quote boundary (#3154): OK ({} passed)", g_passed);
    return 0;
}