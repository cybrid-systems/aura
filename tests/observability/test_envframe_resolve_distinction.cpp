// @category: unit
// @reason: Issue #1890 — EnvFrame invalid vs stale + closure table hygiene
// Issue #1708/#1709/#1754/#1756/#1890 (#1978 renamed): issue# moved from filename to header.
//
// Consolidates / extends #1754 / #1756 / #1708 / #1709:
//   AC1: resolve_env_frame_detailed: NULL / OOB / OK / STALE_VERSION
//   AC2: is_env_frame_invalid_id vs is_env_frame_stale never conflate
//   AC3: free_list push-before-freed source order (#1708/#1890)
//   AC4: capture multi-vector bounds (#1709/#1890)
//   AC5: query:envframe-resolve-distinction-stats schema 1890
//   AC6: free+alloc reuses slot; desync API present

#include "test_harness.hpp"
#include "compiler/runtime_shared.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::EnvFrameResolveStatus;
using aura::compiler::EnvId;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:envframe-resolve-distinction-stats\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void ac1_detailed_statuses() {
    std::println("\n--- AC1: resolve_env_frame_detailed statuses ---");
    Evaluator ev;
    auto n = ev.resolve_env_frame_detailed(NULL_ENV_ID);
    CHECK(n.status == EnvFrameResolveStatus::NULL_ID, "NULL_ID");
    CHECK(n.frame == nullptr, "NULL frame");

    auto o = ev.resolve_env_frame_detailed(999999);
    CHECK(o.status == EnvFrameResolveStatus::OOB, "OOB");
    CHECK(o.frame == nullptr, "OOB frame");

    EnvId id = ev.alloc_env_frame();
    auto ok = ev.resolve_env_frame_detailed(id);
    CHECK(ok.status == EnvFrameResolveStatus::OK, "fresh OK");
    CHECK(ok.frame != nullptr, "fresh frame");
    CHECK(static_cast<bool>(ok), "bool OK");

    // Force version stale without INVALID_VERSION.
    ev.bump_defuse_version_for_test();
    auto st = ev.resolve_env_frame_detailed(id);
    CHECK(st.status == EnvFrameResolveStatus::STALE_VERSION, "STALE_VERSION");
    CHECK(st.frame != nullptr, "stale still has frame for refresh");
    CHECK(!static_cast<bool>(st), "bool false for stale");
}

void ac2_invalid_vs_stale() {
    std::println("\n--- AC2: invalid_id vs stale predicates ---");
    Evaluator ev;
    CHECK(ev.is_env_frame_invalid_id(NULL_ENV_ID), "NULL invalid_id");
    CHECK(!ev.is_env_frame_stale(NULL_ENV_ID), "NULL not stale");
    CHECK(ev.is_env_frame_invalid_id(123456), "OOB invalid_id");
    CHECK(!ev.is_env_frame_stale(123456), "OOB not stale");

    EnvId id = ev.alloc_env_frame();
    CHECK(!ev.is_env_frame_invalid_id(id), "live id valid");
    CHECK(!ev.is_env_frame_stale(id), "fresh not stale");
    ev.bump_defuse_version_for_test();
    CHECK(!ev.is_env_frame_invalid_id(id), "still valid id after defuse bump");
    CHECK(ev.is_env_frame_stale(id), "version-stale after defuse bump");
}

void ac3_free_list_source_order() {
    std::println("\n--- AC3: free_list push before freed=1 ---");
    std::string src;
    for (const char* p :
         {"src/compiler/aura_jit_runtime.cpp", "../src/compiler/aura_jit_runtime.cpp"}) {
        src = read_file(p);
        if (!src.empty())
            break;
    }
    CHECK(!src.empty(), "read aura_jit_runtime.cpp");
    auto pos = src.find("void aura_free_closure");
    CHECK(pos != std::string::npos, "found free");
    auto win = src.substr(pos, 2200);
    CHECK(win.find("#1708") != std::string::npos || win.find("#1890") != std::string::npos,
          "cites free-list issue");
    auto push = win.find("g_closure_free_list.push_back");
    auto freed = win.find("g_closure_freed[cid] = 1");
    CHECK(push != std::string::npos && freed != std::string::npos, "has push + freed");
    CHECK(push < freed, "push before freed=1");
}

void ac4_capture_bounds_source() {
    std::println("\n--- AC4: capture multi-vector bounds ---");
    std::string src;
    for (const char* p :
         {"src/compiler/aura_jit_runtime.cpp", "../src/compiler/aura_jit_runtime.cpp"}) {
        src = read_file(p);
        if (!src.empty())
            break;
    }
    CHECK(!src.empty(), "read runtime");
    CHECK(src.find("closure_slot_in_bounds") != std::string::npos, "slot_in_bounds");
    CHECK(src.find("closure_vectors_consistent") != std::string::npos ||
              src.find("vector_desync") != std::string::npos,
          "desync check");
    auto cpos = src.find("void aura_closure_capture");
    CHECK(cpos != std::string::npos, "capture fn");
    auto cwin = src.substr(cpos, 1200);
    CHECK(cwin.find("closure_slot_in_bounds") != std::string::npos ||
              cwin.find("func_ids") != std::string::npos,
          "capture checks multi-vector");
}

void ac5_query(CompilerService& cs) {
    std::println("\n--- AC5: query schema 1890 ---");
    // Drive distinguished resolves
    auto& ev = cs.evaluator();
    (void)ev.resolve_env_frame_detailed(NULL_ENV_ID);
    EnvId id = ev.alloc_env_frame();
    (void)ev.resolve_env_frame_detailed(id);
    ev.bump_defuse_version_for_test();
    (void)ev.resolve_env_frame_detailed(id);

    auto h = cs.eval("(engine:metrics \"query:envframe-resolve-distinction-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1890, "schema 1890");
    CHECK(href(cs, "active") == 1, "active");
    CHECK(href(cs, "has-stale-version-status") == 1, "STALE flag");
    CHECK(href(cs, "has-invalid-id-predicate") == 1, "invalid_id flag");
    CHECK(href(cs, "free-list-push-before-freed") == 1, "free order flag");
    CHECK(href(cs, "capture-multi-vector-bounds") == 1, "capture flag");
    CHECK(href(cs, "invalid-vs-stale-distinguished") >= 1, "metric bumped");
    CHECK(href(cs, "closure-table-desync-prevented") >= 0, "desync key");
}

void ac6_free_reuse_and_api() {
    std::println("\n--- AC6: free/alloc reuse + desync API ---");
    const auto c0 = aura_alloc_closure(1);
    CHECK(c0 >= 0, "alloc");
    aura_free_closure(c0);
    CHECK(aura_closure_is_freed(c0) == 1, "freed");
    const auto c1 = aura_alloc_closure(2);
    CHECK(c1 == c0, "reused slot");
    CHECK(aura_closure_is_freed(c1) == 0, "live again");
    // Capture into live slot ok
    aura_closure_capture(c1, 0, 42);
    // Capture into freed is no-op (no crash)
    aura_free_closure(c1);
    aura_closure_capture(c1, 0, 99);
    CHECK(aura_closure_table_vector_desync_prevented_total() >= 0, "desync API");
}

} // namespace

int main() {
    std::println("=== Issue #1890: EnvFrame resolve distinction + closure table ===");
    ac1_detailed_statuses();
    ac2_invalid_vs_stale();
    ac3_free_list_source_order();
    ac4_capture_bounds_source();
    {
        CompilerService cs;
        ac5_query(cs);
    }
    ac6_free_reuse_and_api();
    std::println("\n=== #1890: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
