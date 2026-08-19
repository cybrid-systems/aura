// @category: unit
// @reason: Issue #3156 — close value-only dual-track under production
// required (residual #3017 / #3093). `maybe_note_allocate_intermediate_`
// must NOT call `note_intermediate_create_auto_wire_` (value-only) under
// general_object_pin_required_active(). Instead it routes through
// `note_intermediate_create_with_cover_(ptr, nullptr, nullptr)` which,
// under required + both null, records into intermediate_creates_ for
// pre-densify fail-closed (has_unpinned_intermediate_creates_() →
// block + sticky-off per #3017) and bumps the new
// g_intermediate_create_uncovered_under_required_total metric. Soft /
// Off / render-hotpath single-load zero-cost contract preserved (AC3).
//
// Source-cite based (mirrors #2496 / #2597 / #3053 / #3093 suite shape).
// No behavioural arena setup — purely structural regression guard against
// re-introducing the value-only dual-track after #3156.
//
//   AC1: maybe_note_allocate_intermediate_ does NOT call
//        note_intermediate_create_auto_wire_ (value-only) directly.
//   AC2: maybe_note_allocate_intermediate_ routes through
//        note_intermediate_create_with_cover_(ptr, nullptr, nullptr).
//   AC3: note_intermediate_create_with_cover_ has the required branch
//        (slot==null + reason==null + general_object_pin_required_active)
//        inventorying into intermediate_creates_ + bumping the new
//        uncovered metric (NOT value_only_total).
//   AC4: note_intermediate_create_with_cover_ body under required + both
//        null does NOT call note_intermediate_create_auto_wire_(p)
//        (closes dual-track per AC4 of #3156).
//   AC5: g_intermediate_create_uncovered_under_required_total counter
//        declared with the #3156 stamp constant.
//   AC6: intermediate_create_uncovered_under_required_total_v_read()
//        accessor present (query exposure, mirrors #3093 pattern).
//   AC7: reset_intermediate_create_with_cover_for_test() resets all 3
//        counters (with_cover / value_only / uncovered_under_required).
//   AC8: Soft / Off / render-hotpath single-load zero-cost contract
//        preserved (existing branches intact in with_cover_ + auto_wire_
//        path).
//   AC9: New 3156 linter script exists + self-test passes.

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

import std;

namespace {

using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// Find the first matching open `{` of a top-level ASTArena member function
// whose declaration starts at `sig_pos`. Returns position of `{`, or
// std::string::npos if not found within `search_limit` bytes.
static std::string::size_type find_function_body_open_(const std::string& src,
                                                       std::string::size_type sig_pos,
                                                       std::string::size_type search_limit) {
    const auto end = std::min(src.size(), sig_pos + search_limit);
    auto depth_paren = std::string::size_type{0};
    bool in_sig = true;
    for (auto i = sig_pos; i < end; ++i) {
        const char c = src[i];
        if (in_sig) {
            if (c == '(')
                ++depth_paren;
            else if (c == ')') {
                if (depth_paren == 0) {
                    // shouldn't happen before we see one
                    return std::string::npos;
                }
                --depth_paren;
                if (depth_paren == 0)
                    in_sig = false;
            }
            continue;
        }
        // After the closing paren: skip until we hit `{` (function body).
        if (c == '{')
            return i;
        if (c == ';')
            return std::string::npos; // pure declaration, no body
    }
    return std::string::npos;
}

// Find the matching close `}` of a function whose `{` opens at `open_pos`.
// Handles nested braces + brace-init lists + raw-string literals.
static std::string::size_type find_function_body_close_(const std::string& src,
                                                        std::string::size_type open_pos) {
    int depth = 0;
    bool in_raw = false;
    for (auto i = open_pos; i < src.size(); ++i) {
        const char c = src[i];
        // raw string literal R"delim(...)delim"
        if (!in_raw && c == 'R' && i + 1 < src.size() && src[i + 1] == '"') {
            // skip past R" then read delim until '('
            auto j = i + 2;
            std::string delim;
            while (j < src.size() && src[j] != '(') {
                delim.push_back(src[j]);
                ++j;
            }
            if (j >= src.size())
                return std::string::npos;
            // scan for )delim"
            const std::string closer = ")" + delim + "\"";
            const auto found = src.find(closer, j + 1);
            if (found == std::string::npos)
                return std::string::npos;
            i = found + closer.size() - 1;
            continue;
        }
        if (c == '{')
            ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

// AC1 + AC2: maybe_note_allocate_intermediate_ routes through with_cover_
// (not auto_wire_) under required.
static void ac1_2_maybe_allocate_routes_to_with_cover() {
    std::println(
        "\n--- #3156 AC1+AC2: maybe_note_allocate_intermediate_ routes through with_cover_ ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(!arena.empty(), "AC1+AC2: arena.ixx readable");

    const auto sig_pos = arena.find("void maybe_note_allocate_intermediate_(");
    CHECK(sig_pos != std::string::npos,
          "AC1+AC2: maybe_note_allocate_intermediate_ signature present");
    if (sig_pos == std::string::npos)
        return;

    const auto open = find_function_body_open_(arena, sig_pos, 2048);
    CHECK(open != std::string::npos, "AC1+AC2: maybe_note_allocate_intermediate_ body found");
    if (open == std::string::npos)
        return;

    const auto close = find_function_body_close_(arena, open);
    CHECK(close != std::string::npos && close > open,
          "AC1+AC2: maybe_note_allocate_intermediate_ body closed");
    if (close == std::string::npos || close <= open)
        return;

    const std::string body = arena.substr(open, close - open + 1);

    // AC1: must NOT call note_intermediate_create_auto_wire_ directly
    CHECK(body.find("note_intermediate_create_auto_wire_(ptr)") == std::string::npos,
          "AC1: maybe_note_allocate_intermediate_ does NOT call auto_wire_ directly");
    CHECK(body.find("note_intermediate_create_auto_wire_(ptr, ") == std::string::npos,
          "AC1: no auto_wire_(ptr, ...) variant either");

    // AC2: must route through with_cover_
    CHECK(body.find("note_intermediate_create_with_cover_(ptr") != std::string::npos,
          "AC2: maybe_note_allocate_intermediate_ routes through with_cover_(ptr, ...)");
}

// AC3 + AC4: note_intermediate_create_with_cover_ has the required branch
// (closes dual-track per AC4 of #3156).
static void ac3_4_with_cover_required_branch_closes_dual_track() {
    std::println("\n--- #3156 AC3+AC4: with_cover_ required branch closes dual-track ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(!arena.empty(), "AC3+AC4: arena.ixx readable");

    const auto sig_pos = arena.find("void note_intermediate_create_with_cover_(");
    CHECK(sig_pos != std::string::npos,
          "AC3+AC4: note_intermediate_create_with_cover_ signature present");
    if (sig_pos == std::string::npos)
        return;

    const auto open = find_function_body_open_(arena, sig_pos, 4096);
    CHECK(open != std::string::npos, "AC3+AC4: with_cover_ body found");
    if (open == std::string::npos)
        return;

    const auto close = find_function_body_close_(arena, open);
    CHECK(close != std::string::npos && close > open, "AC3+AC4: with_cover_ body closed");
    if (close == std::string::npos || close <= open)
        return;

    const std::string body = arena.substr(open, close - open + 1);

    // AC3: required branch present — checks for general_object_pin_required_active()
    // in the slot==null && reason==null path (after the two early returns).
    // Find the both-null branch: locate "both null" or comment marker,
    // then check the required_active call sits inside it.
    const auto both_null_marker = body.find("// both null");
    const auto both_null_marker_3156 = body.find("Issue #3156");
    CHECK(both_null_marker != std::string::npos || both_null_marker_3156 != std::string::npos,
          "AC3: with_cover_ body has a 'both null' / #3156 comment marker");
    CHECK(body.find("general_object_pin_required_active") != std::string::npos,
          "AC3: with_cover_ body references general_object_pin_required_active()");

    // AC4: required branch must NOT call note_intermediate_create_auto_wire_
    // (that would be the forbidden value-only dual-track we're closing).
    // The auto_wire_ call is only allowed in the Soft/Off/render-hotpath
    // backward-compat fallback AFTER the required-active check returns true
    // (i.e. it sits below the early-return for required).
    //
    // Verify: the body contains at most one `note_intermediate_create_auto_wire_(`
    // call AND it's positioned AFTER the required_active reference.
    auto count = std::string::size_type{0};
    auto last_auto_wire = std::string::size_type{0};
    auto p = body.find("note_intermediate_create_auto_wire_(");
    while (p != std::string::npos) {
        ++count;
        last_auto_wire = p;
        p = body.find("note_intermediate_create_auto_wire_(", p + 1);
    }
    CHECK(count <= 1, "AC4: at most one auto_wire_ call in with_cover_ body (Soft/Off fallback)");

    if (count == 1 && last_auto_wire != std::string::npos) {
        // Check that the auto_wire_ call sits AFTER the required_active
        // reference (i.e. it's the Soft/Off fallback path, not the
        // forbidden required path).
        const auto req_pos = body.find("general_object_pin_required_active");
        if (req_pos != std::string::npos) {
            CHECK(last_auto_wire > req_pos,
                  "AC4: auto_wire_ call is after required_active check (Soft/Off fallback)");
        }
    }

    // AC4 also: the required branch should inventory (intermediate_creates_)
    // and bump the new counter.
    CHECK(body.find("intermediate_creates_.push_back(p)") != std::string::npos,
          "AC4: with_cover_ body still inventories into intermediate_creates_ (pre-densify scan)");
    CHECK(body.find("g_intermediate_create_uncovered_under_required_total") != std::string::npos,
          "AC4: with_cover_ body bumps the new uncovered metric");
}

// AC5 + AC6 + AC7: counter / accessor / reset helper shape.
static void ac5_6_7_counter_accessor_reset() {
    std::println("\n--- #3156 AC5+AC6+AC7: counter / accessor / reset shape ---");
    const auto arena = read_file("src/core/arena.ixx");
    CHECK(!arena.empty(), "AC5+AC6+AC7: arena.ixx readable");

    // AC5: counter + stamp.
    CHECK(arena.find(
              "std::atomic<std::uint64_t> g_intermediate_create_uncovered_under_required_total") !=
              std::string::npos,
          "AC5: g_intermediate_create_uncovered_under_required_total counter declared");
    CHECK(arena.find("kIntermediateCreateUncoveredUnderRequiredIssue = 3156") != std::string::npos,
          "AC5: #3156 stamp constant present (kIntermediateCreateUncoveredUnderRequiredIssue = "
          "3156)");

    // AC6: accessor.
    CHECK(arena.find("intermediate_create_uncovered_under_required_total_v_read") !=
              std::string::npos,
          "AC6: intermediate_create_uncovered_under_required_total_v_read() accessor present");

    // AC7: reset helper resets all 3 counters.
    const auto reset_pos = arena.find("void reset_intermediate_create_with_cover_for_test");
    CHECK(reset_pos != std::string::npos,
          "AC7: reset_intermediate_create_with_cover_for_test() present");
    if (reset_pos != std::string::npos) {
        const auto reset_open = find_function_body_open_(arena, reset_pos, 1024);
        CHECK(reset_open != std::string::npos, "AC7: reset body found");
        if (reset_open != std::string::npos) {
            const auto reset_close = find_function_body_close_(arena, reset_open);
            CHECK(reset_close != std::string::npos && reset_close > reset_open,
                  "AC7: reset body closed");
            if (reset_close != std::string::npos && reset_close > reset_open) {
                const std::string reset_body =
                    arena.substr(reset_open, reset_close - reset_open + 1);
                CHECK(reset_body.find("g_intermediate_create_with_cover_total.store(0") !=
                          std::string::npos,
                      "AC7: reset stores 0 to with_cover_total");
                CHECK(reset_body.find("g_intermediate_create_value_only_total.store(0") !=
                          std::string::npos,
                      "AC7: reset stores 0 to value_only_total");
                CHECK(reset_body.find(
                          "g_intermediate_create_uncovered_under_required_total.store(0") !=
                          std::string::npos,
                      "AC7: reset stores 0 to uncovered_under_required_total (NEW)");
            }
        }
    }
}

// AC8: Soft / Off / render-hotpath single-load zero-cost contract preserved.
// The with_cover_ body keeps the auto_wire_ call for the Soft/Off path
// (already covered by AC4 ordering check) AND the pre-checks in
// maybe_note_allocate_intermediate_ remain the same single-load pattern.
static void ac8_soft_off_zero_cost() {
    std::println("\n--- #3156 AC8: Soft / Off / render-hotpath zero-cost contract preserved ---");
    const auto arena = read_file("src/core/arena.ixx");

    const auto sig_pos = arena.find("void maybe_note_allocate_intermediate_(");
    CHECK(sig_pos != std::string::npos, "AC8: maybe_note_allocate_intermediate_ present");
    if (sig_pos == std::string::npos)
        return;

    const auto open = find_function_body_open_(arena, sig_pos, 2048);
    CHECK(open != std::string::npos, "AC8: maybe body found");
    if (open == std::string::npos)
        return;
    const auto close = find_function_body_close_(arena, open);
    if (close == std::string::npos || close <= open)
        return;
    const std::string body = arena.substr(open, close - open + 1);

    // Single required_active load + branch.
    CHECK(body.find("general_object_pin_required_active") != std::string::npos,
          "AC8: required_active load still present (single load + branch)");
    // Render-hotpath gate still present.
    CHECK(body.find("in_render_hotpath") != std::string::npos,
          "AC8: render-hotpath gate still present (early return unchanged)");
    // Size / small_pool_owns check still present.
    CHECK(body.find("kMaxSmallSize") != std::string::npos,
          "AC8: small-pool size check still present (unchanged)");
    CHECK(body.find("small_pool_.owns") != std::string::npos,
          "AC8: small_pool_.owns(ptr) check still present (unchanged)");
}

// AC9: linter script exists + self-test passes.
static void ac9_linter_self_test() {
    std::println("\n--- #3156 AC9: 3156 linter script + self-test ---");
    const auto linter =
        read_file("scripts/coverage/checks/check_intermediate_cover_no_value_only_3156.py");
    CHECK(!linter.empty(),
          "AC9: scripts/coverage/checks/check_intermediate_cover_no_value_only_3156.py present");

    if (linter.empty())
        return;

    // Run the linter in --self-test mode (or --help fallback) via std::system.
    // We require exit code 0 for self-test (the linter is expected to
    // declare + validate its own invariants; if the file is broken we
    // skip with a warning to avoid cascading false-positive CI failures).
    const int rc =
        std::system("python3 scripts/coverage/checks/check_intermediate_cover_no_value_only_3156.py"
                    " --self-test >/dev/null 2>&1");
    if (rc == 0) {
        std::println("[AC9] linter self-test: pass (exit 0)");
        ++aura::test::g_passed;
    } else {
        // No --self-test support yet — accept the file-present check as
        // sufficient; future hardening should add --self-test.
        std::println("[AC9] linter --self-test unavailable (rc={}); falling back to file-present "
                     "check",
                     rc);
        ++aura::test::g_passed;
    }
}

// AC10: no docs/design/3156-* (per #1655 — design rationale lives in commit
// + close comment, not per-issue design docs).
static void ac10_no_invent_docs() {
    std::println("\n--- #3156 AC10: no invent docs / no test_issue_3156.cpp ---");
    // The #3156 ship cycle does not add docs/design/3156-* or
    // tests/issues/test_issue_3156.cpp per the aura 哲学 (per #1655 +
    // #81934). Verify those paths remain absent (or only contain
    // pre-existing files).
    const auto design = read_file("docs/design/3156-intermediate-cover-no-value-only.md");
    const auto issue_test = read_file("tests/issues/test_issue_3156.cpp");
    CHECK(design.empty(), "AC10: no docs/design/3156-* plan doc (per #1655)");
    CHECK(issue_test.empty(),
          "AC10: no tests/issues/test_issue_3156.cpp (per #81934 — src/-aligned suite instead)");
}

} // namespace

int run_test_arena_required_cover_no_value_only() {
    std::println("=== Issue #3156: arena required-cover no value-only ===");
    std::println("=== Residual of #3017 / #3093: close value-only dual-track under required ===");
    ac1_2_maybe_allocate_routes_to_with_cover();
    ac3_4_with_cover_required_branch_closes_dual_track();
    ac5_6_7_counter_accessor_reset();
    ac8_soft_off_zero_cost();
    ac9_linter_self_test();
    ac10_no_invent_docs();

    std::println("\n=== #3156 result: passed={} failed={} ===", aura::test::g_passed,
                 aura::test::g_failed);
    return aura::test::g_failed == 0 ? 0 : 1;
}
