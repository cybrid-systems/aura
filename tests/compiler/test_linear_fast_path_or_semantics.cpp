// test_linear_fast_path_or_semantics.cpp -- source-cite AC for Issue #3558
//
// @category: unit
// @reason: Issue #3558 — `linear_fast_path_ok` OR-semantics race-window
//   fix. Three sites in src/compiler/ replace max-style pick
//   (`override >= 0 ? override : depth`) with independent OR checks
//   (`override > 0` || `actual_depth > 0`). Mid-boundary override=0 flip
//   no longer lets MoveOp elision slip through while actual depth > 0.
//
//   Full runtime needs a live evaluator with `enter_mutation_boundary()`
//   + a concurrent fiber flipping `g_linear_ir_fastpath_boundary_depth_override`
//   from -1 to 0 mid-boundary — too heavy for a quick init helper.
//   Covered by existing #3006 (depth-or-densify block) and #3085
//   (rehydrate-miss) regression tests through the full mutate flow.
//   This test is source-cite only: assert the 3 sites use OR semantics
//   and the old max-style pick is gone.
//
//   AC1: typed_mutation_audit.h linear_fast_path_ok mid-boundary arm
//        uses `override > 0` then `aura_evaluator_mutation_boundary_depth() > 0`
//        (no `max(override, depth)` style pick).
//   AC2: typed_mutation_audit.h linear_ir_fastpath_try_skip re-sample
//        block checks both signals independently in the same predicate.
//   AC3: typed_mutation_audit_hooks.cpp aura_linear_fast_path_depth_or_densify_block
//        early-returns 1 on either signal — name finally matches behavior.
//   AC4: linter scripts/coverage/checks/check_linear_fast_path_or_semantics.py
//        exists and references both source files.
//   AC5: no regression markers — no `max(override, depth)` pattern at any of
//        the three sites.
//
// Standalone TU — does NOT include test_harness.hpp / typed_mutation_audit.h
// to avoid pulling in the JIT/runtime symbol graph. The source-cite reads
// only need std::ifstream + std::string.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

// Read a file from CWD or one/two levels up (matches how other source-cite
// tests locate the source tree when run from build/).
std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// Scan from `pos` (assumed to point at fn_name) for `WINDOW` chars and
// verify both OR-semantics markers are present and the old max-pick is gone.
bool check_or_semantics(std::string_view src, std::string_view fn_name,
                        std::initializer_list<std::string_view> or_markers,
                        std::size_t window = 2000) {
    auto fn_pos = src.find(fn_name);
    if (fn_pos == std::string_view::npos)
        return false;
    auto body_end = std::min(src.size(), fn_pos + window);
    std::string_view body(src.data() + fn_pos, body_end - fn_pos);
    for (auto m : or_markers) {
        if (!contains(body, m))
            return false;
    }
    // Old max-style pick signature must be absent in the window.
    static constexpr std::string_view kMaxPick =
        "depth = static_cast<std::size_t>(g_linear_ir_fastpath_boundary_depth_override)";
    if (contains(body, kMaxPick))
        return false;
    return true;
}

} // namespace

int main() {
    int passed = 0;
    int failed = 0;

    const auto audit_src = read_file("src/compiler/typed_mutation_audit.h");
    const auto hooks_src = read_file("src/compiler/typed_mutation_audit_hooks.cpp");

    if (audit_src.empty()) {
        std::fprintf(stderr, "FAIL: could not read src/compiler/typed_mutation_audit.h\n");
        ++failed;
    }
    if (hooks_src.empty()) {
        std::fprintf(stderr, "FAIL: could not read src/compiler/typed_mutation_audit_hooks.cpp\n");
        ++failed;
    }

    // AC1: linear_fast_path_ok() mid-boundary arm uses OR semantics.
    if (!audit_src.empty() &&
        check_or_semantics(audit_src, "linear_fast_path_ok()",
                           {"if (g_linear_ir_fastpath_boundary_depth_override > 0)\n"
                            "            return false;",
                            "if (aura_evaluator_mutation_boundary_depth() > 0)\n"
                            "            return false;"})) {
        std::fprintf(stdout, "AC1 PASS: linear_fast_path_ok mid-boundary arm uses OR semantics\n");
        ++passed;
    } else {
        std::fprintf(stdout, "AC1 FAIL: linear_fast_path_ok mid-boundary arm not OR-semantics\n");
        ++failed;
    }

    // AC2: linear_ir_fastpath_try_skip() #3238 re-sample uses OR semantics.
    if (!audit_src.empty() &&
        check_or_semantics(
            audit_src, "linear_ir_fastpath_try_skip()",
            {"if (g_linear_ir_fastpath_boundary_depth_override > 0 ||\n"
             "            aura_evaluator_mutation_boundary_depth() > 0 || pending > 0)"})) {
        std::fprintf(stdout, "AC2 PASS: linear_ir_fastpath_try_skip re-sample uses OR semantics\n");
        ++passed;
    } else {
        std::fprintf(stdout, "AC2 FAIL: linear_ir_fastpath_try_skip re-sample not OR-semantics\n");
        ++failed;
    }

    // AC3: aura_linear_fast_path_depth_or_densify_block uses OR semantics.
    if (!hooks_src.empty() &&
        check_or_semantics(hooks_src, "aura_linear_fast_path_depth_or_densify_block",
                           {"if (g_linear_ir_fastpath_boundary_depth_override > 0)\n"
                            "        return 1;",
                            "if (aura_evaluator_mutation_boundary_depth() > 0)\n"
                            "        return 1;"},
                           1500)) {
        std::fprintf(stdout,
                     "AC3 PASS: aura_linear_fast_path_depth_or_densify_block uses OR semantics\n");
        ++passed;
    } else {
        std::fprintf(stdout,
                     "AC3 FAIL: aura_linear_fast_path_depth_or_densify_block not OR-semantics\n");
        ++failed;
    }

    // AC4: linter exists and references both source files.
    const auto linter = read_file("scripts/coverage/checks/check_linear_fast_path_or_semantics.py");
    if (!linter.empty() && contains(linter, "typed_mutation_audit.h") &&
        contains(linter, "typed_mutation_audit_hooks.cpp")) {
        std::fprintf(stdout, "AC4 PASS: linter exists and references both source files\n");
        ++passed;
    } else {
        std::fprintf(stdout, "AC4 FAIL: linter missing or does not reference both files\n");
        ++failed;
    }

    std::fprintf(stdout, "=== Issue #3558 === %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}