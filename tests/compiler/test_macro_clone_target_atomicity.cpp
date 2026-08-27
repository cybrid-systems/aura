// @category: unit
// @reason: Issue #3157 — depth/steal fail in clone_macro_body only rolled
// name_map; orphan MacroIntroduced nodes allocated via target.add_* during
// the recursive clone walk remained reachable via size growth / free-list
// gaps even though nm_ckpt rolled back the rename table. Production
// (Restricted/Strict sandbox active) fail-closed via existing
// install_macro_expand_checkpoint() / ExpandCheckpointGuard machinery from
// the #3062 family (same helpers used by macro_expand_all_body pass-limit
// rollback). Soft / Off keeps historical half-write (zero-cost contract).
//
//   AC1: clone_macro_body_at_depth installs ExpandCheckpointGuard at
//        top-level entry (hygiene_depth == 0) gated on production surface
//        (aura::core::sandbox::is_sandbox_active()).
//   AC2: steal-fail return path (steal1 > steal0, last_reject_reason=3)
//        calls expand_ckpt.try_restore() BEFORE return NULL_NODE, so
//        target.add_* allocations are rolled back.
//   AC3: Soft / Off contract preserved — production_surface check gates
//        ensure_installed(); no checkpoint install under Soft.
//   AC4: g_macro_clone_steal_abort_total + g_macro_clone_last_reject_reason
//        still fire on steal-fail (existing counters preserved).
//   AC5: No new file-level atomic — the existing expand checkpoint
//        machinery from #3062 family is reused (not a second rollback
//        engine per issue non-goals).
//   AC6: Nested recursion (hygiene_depth > 0) does NOT install a new
//        checkpoint — the top-level owns the rollback for the whole
//        subtree (no double-rollback).
//   AC7: No docs/design/3157-* plan doc (per #1655 aura 哲学).
//   AC8: No tests/issues/test_issue_3157.cpp (per #81934 — src/-aligned
//        suite instead).
//
// Sibling tests must remain green:
//   - tests/compiler/test_pre_scan_quote_boundary.cpp (#3154)
//   - tests/compiler/test_compact_nodes_provenance_schema_remap.cpp (#3155)
//   - tests/compiler/test_rest_param_hygiene_eval_flat.cpp (#3153)
//   - tests/compiler/test_macro_hygiene_limits.cpp (#3062 family)
//   - tests/compiler/test_steal_checkpoint_residual_2890.cpp (#2890)

#include "test_harness.hpp"

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

// Find the first matching `{` of a function whose declaration starts at
// `sig_pos`. Mirrors the helper used by other src/-aligned macro tests.
// Returns position of `{`, or std::string::npos if not found within
// `search_limit` bytes.
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
                    return std::string::npos;
                }
                --depth_paren;
                if (depth_paren == 0)
                    in_sig = false;
            }
            continue;
        }
        if (c == '{')
            return i;
        if (c == ';')
            return std::string::npos;
    }
    return std::string::npos;
}

static std::string::size_type find_function_body_close_(const std::string& src,
                                                        std::string::size_type open_pos) {
    int depth = 0;
    bool in_raw = false;
    for (auto i = open_pos; i < src.size(); ++i) {
        const char c = src[i];
        if (!in_raw && c == 'R' && i + 1 < src.size() && src[i + 1] == '"') {
            auto j = i + 2;
            std::string delim;
            while (j < src.size() && src[j] != '(') {
                delim.push_back(src[j]);
                ++j;
            }
            if (j >= src.size())
                return std::string::npos;
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

// AC1 + AC2 + AC6: clone_macro_body_at_depth installs ExpandCheckpointGuard
// at top-level entry (hygiene_depth == 0) gated on production surface;
// steal-fail return path calls try_restore() before return NULL_NODE.
static void ac1_2_6_clone_at_depth_installs_and_restores() {
    std::println("\n--- #3157 AC1+AC2+AC6: clone_macro_body_at_depth installs expand ckpt "
                 "+ try_restore on steal-fail ---");
    const auto macro_exp = read_file("src/compiler/macro_expansion.cpp");
    CHECK(!macro_exp.empty(), "AC1+AC2+AC6: macro_expansion.cpp readable");

    // AC1: ExpandCheckpointGuard struct definition lives inside clone_macro_body_at_depth
    // (after NameMapCheckpoint, mirroring the macro_expand_all_body pattern).
    // Forward decl then definition; rfind the definition (has a body).
    const auto sig_pos = macro_exp.rfind("static aura::ast::NodeId clone_macro_body_at_depth(");
    CHECK(sig_pos != std::string::npos, "AC1+AC2+AC6: clone_macro_body_at_depth signature present");
    if (sig_pos == std::string::npos)
        return;

    // Find the body — use a generous search limit since the function is large.
    const auto open = find_function_body_open_(macro_exp, sig_pos, 16384);
    CHECK(open != std::string::npos, "AC1+AC2+AC6: clone_macro_body_at_depth body found");
    if (open == std::string::npos)
        return;

    // Bound the install-site scan to this function. ExpandCheckpointGuard
    // sits after NameMapCheckpoint (~20 KB past `{`); a fixed 8 KB window
    // misses it and can also pick up the later macro_expand_all_body copy.
    const auto close = find_function_body_close_(macro_exp, open);
    CHECK(close != std::string::npos, "AC1+AC2+AC6: clone_macro_body_at_depth body closes");
    if (close == std::string::npos)
        return;

    // AC1: ExpandCheckpointGuard struct + ensure_installed call gated on
    // hygiene_depth == 0 && production_surface.
    const auto ecg_struct_pos = macro_exp.find("ExpandCheckpointGuard", open);
    CHECK(ecg_struct_pos != std::string::npos && ecg_struct_pos < close,
          "AC1: ExpandCheckpointGuard struct present in clone_macro_body_at_depth");
    const auto install_pos = macro_exp.find("expand_ckpt.ensure_installed", open);
    CHECK(install_pos != std::string::npos && install_pos < close,
          "AC1: expand_ckpt.ensure_installed call present");
    if (install_pos != std::string::npos) {
        const auto prod_pos = macro_exp.find("production_surface", install_pos - 1024);
        CHECK(prod_pos != std::string::npos && prod_pos < install_pos,
              "AC1: production_surface gates ensure_installed (Soft/Off zero-cost)");
        const auto depth_pos =
            macro_exp.find("hygiene_depth == 0", std::max(sig_pos, install_pos - 1024));
        CHECK(depth_pos != std::string::npos && depth_pos < install_pos,
              "AC1: install is top-level only (hygiene_depth == 0)");
    }

    // AC2: steal-fail return path calls try_restore() before return NULL_NODE.
    const auto steal_check_pos = macro_exp.find("if (steal1 > steal0)");
    CHECK(steal_check_pos != std::string::npos, "AC2: steal check (if (steal1 > steal0)) present");
    if (steal_check_pos != std::string::npos) {
        const auto try_restore_pos = macro_exp.find("expand_ckpt.try_restore", steal_check_pos);
        CHECK(try_restore_pos != std::string::npos,
              "AC2: expand_ckpt.try_restore() called in steal-fail path");
        const auto return_null_pos = macro_exp.find("return NULL_NODE", steal_check_pos);
        if (try_restore_pos != std::string::npos && return_null_pos != std::string::npos) {
            CHECK(try_restore_pos < return_null_pos,
                  "AC2: try_restore() is BEFORE return NULL_NODE (fail-closed ordering)");
        }
    }

    // AC6: Nested recursion (hygiene_depth > 0) does NOT install a new
    // checkpoint — the gate `hygiene_depth == 0` ensures the top-level
    // owns the rollback for the whole subtree.
    if (install_pos != std::string::npos) {
        const auto gate_str = "if (hygiene_depth == 0 && production_surface)";
        const auto gate_pos = macro_exp.find(gate_str, std::max(sig_pos, install_pos - 512));
        CHECK(gate_pos != std::string::npos && gate_pos <= install_pos + 64,
              "AC6: install gated on hygiene_depth == 0 (nested recursion inherits top-level)");
    }
}

// AC3: Soft / Off contract preserved — production_surface check gates
// ensure_installed(); no checkpoint install under Soft.
static void ac3_soft_off_zero_cost() {
    std::println("\n--- #3157 AC3: Soft / Off contract preserved ---");
    const auto macro_exp = read_file("src/compiler/macro_expansion.cpp");

    // AC3: production_surface is computed via aura::core::sandbox::is_sandbox_active()
    // and used as the gate for ensure_installed() — Soft / Off keep
    // historical half-write (no checkpoint, no restore).
    const auto prod_surface_pos =
        macro_exp.find("const bool production_surface = aura::core::sandbox::is_sandbox_active()");
    CHECK(prod_surface_pos != std::string::npos,
          "AC3: production_surface computed via sandbox::is_sandbox_active()");
    const auto gate_pos = macro_exp.find(
        "if (hygiene_depth == 0 && production_surface)\n        expand_ckpt.ensure_installed()");
    CHECK(gate_pos != std::string::npos,
          "AC3: ensure_installed() gated on production_surface (Soft / Off skip)");
}

// AC4: g_macro_clone_steal_abort_total + g_macro_clone_last_reject_reason
// still fire on steal-fail (existing counters preserved).
static void ac4_existing_steal_counters_preserved() {
    std::println("\n--- #3157 AC4: existing steal counters preserved ---");
    const auto macro_exp = read_file("src/compiler/macro_expansion.cpp");

    const auto steal_abort_pos = macro_exp.find("g_macro_clone_steal_abort_total.fetch_add(1");
    CHECK(steal_abort_pos != std::string::npos,
          "AC4: g_macro_clone_steal_abort_total counter present (issue-required invariant)");
    const auto reject_reason_pos =
        macro_exp.find("g_macro_clone_last_reject_reason.store(kHygieneLimitReasonStealAbort");
    CHECK(
        reject_reason_pos != std::string::npos,
        "AC4: g_macro_clone_last_reject_reason=steal-abort still fires (issue-required invariant)");

    // Both must be in the steal-fail block.
    if (steal_abort_pos != std::string::npos && reject_reason_pos != std::string::npos) {
        // Steal-fail block anchor: the closest preceding "if (steal1 > steal0)"
        const auto anchor = macro_exp.rfind("if (steal1 > steal0)", steal_abort_pos);
        CHECK(anchor != std::string::npos, "AC4: steal-abort counters inside steal-fail block");
        CHECK(anchor < steal_abort_pos && anchor < reject_reason_pos,
              "AC4: both counters fire inside steal-fail block (after the if check)");
    }
}

// AC5: No new file-level atomic — the existing expand checkpoint machinery
// from #3062 family is reused (not a second rollback engine per issue
// non-goals).
static void ac5_no_new_rollback_engine() {
    std::println("\n--- #3157 AC5: no new file-level atomic, expand ckpt machinery reused ---");
    const auto macro_exp = read_file("src/compiler/macro_expansion.cpp");

    // AC5: No new g_3157_* atomic counters introduced.
    CHECK(macro_exp.find("g_3157_") == std::string::npos,
          "AC5: no g_3157_* atomic counter (existing expand ckpt machinery reused)");
    CHECK(macro_exp.find("atomic<std::uint64_t> g_3157_") == std::string::npos,
          "AC5: no new file-level atomic for #3157 (per issue non-goals)");

    // AC5: install_macro_expand_checkpoint + commit + restore are reused
    // from #3062 family (no second rollback engine).
    CHECK(macro_exp.find("install_macro_expand_checkpoint()") != std::string::npos,
          "AC5: install_macro_expand_checkpoint() reused (#3062 family)");
    CHECK(macro_exp.find("aura_evaluator_try_restore_macro_expand_checkpoint()") !=
              std::string::npos,
          "AC5: aura_evaluator_try_restore_macro_expand_checkpoint reused");
    CHECK(macro_exp.find("aura_evaluator_commit_macro_expand_checkpoint()") != std::string::npos,
          "AC5: aura_evaluator_commit_macro_expand_checkpoint reused");
}

// AC7 + AC8: no invent docs / no test_issue_3157.cpp (per #1655 / #81934).
static void ac7_8_no_invent_docs() {
    std::println("\n--- #3157 AC7+AC8: no invent docs / no test_issue_3157.cpp ---");
    const auto design = read_file("docs/design/3157-macro-clone-target-atomicity.md");
    const auto issue_test = read_file("tests/issues/test_issue_3157.cpp");
    CHECK(design.empty(), "AC7: no docs/design/3157-* plan doc (per #1655 aura 哲学)");
    CHECK(issue_test.empty(),
          "AC8: no tests/issues/test_issue_3157.cpp (per #81934 — src/-aligned suite instead)");

    // AC7 + AC8: src/-aligned suite present (this test file).
    const auto this_test = read_file("tests/compiler/test_macro_clone_target_atomicity.cpp");
    CHECK(!this_test.empty() &&
              this_test.find("run_test_macro_clone_target_atomicity") != std::string::npos,
          "AC7+AC8: src/-aligned test tests/compiler/test_macro_clone_target_atomicity.cpp "
          "present");
}

} // namespace

int run_test_macro_clone_target_atomicity() {
    std::println("=== Issue #3157: clone_macro_body target FlatAST atomicity ===");
    std::println("=== Residual of #2890 / #3028 / #3062: fail-closed under production ===");
    ac1_2_6_clone_at_depth_installs_and_restores();
    ac3_soft_off_zero_cost();
    ac4_existing_steal_counters_preserved();
    ac5_no_new_rollback_engine();
    ac7_8_no_invent_docs();

    std::println("\n=== #3157 result: passed={} failed={} ===", aura::test::g_passed,
                 aura::test::g_failed);
    return aura::test::g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_macro_clone_target_atomicity();
}
#endif
