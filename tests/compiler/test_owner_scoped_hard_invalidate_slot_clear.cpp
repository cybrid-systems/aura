// @category: unit
// @reason: Issue #3377 \u2014 owner-scoped hard invalidate must physically
// clear the owner AOT slot for the mutated define, not just rely on
// the generation-behind predicate. The owner-scoped branch of
// aura_aot_bump_func_table_epoch does NOT advance g_aot_table_epoch
// (preserve #2841/#2951 peer-scope), so aot_invalidate_all_stale_slots_for_eval
// skips every owner slot with table_generation == cur_epoch and the
// invalidate count stays 0. Without this fix, pre-mutate native can
// ride the same table epoch on the owner eval. Non-duplicative to
// #2271/#2299/#2841/#2951/#3070/#3300/#3351.
//
//   AC1: source cites the new aura_aot_invalidate_owner_slot_for_func_id
//        + the call from hard_invalidate_via_facade when epoch didn't move
//        AND multi-eval live > 1
//   AC2: source cites the owner-scope branch (epoch not bumped) + the
//        existing peer-marks (aura_aot_mark_peer_slots_soft_stale +
//        aura_aot_mark_peer_jit_name_soft_stale) are still called
//   AC3: source cites the Soft / Off / single-eval gate
//        (production_defaults_active + map_size > 1) + the Soft early-return
//   AC4: source cites aot_invalidate_all_stale_slots_for_eval
//        generation-behind predicate is unchanged (slot_gen == cur_epoch skip)
//   AC5: no docs/design/3377-*; no test_issue_3377.cpp per #1655 / #81967

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

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

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

int run_test_owner_scoped_hard_invalidate_slot_clear() {
    std::println(
        "=== Issue #3377: owner-scoped hard invalidate physically clears owner AOT slot ===");
    CHECK(true, "3377: issue stamp");

    // \u2500\u2500 AC1: new helper + call from hard_invalidate_via_facade \u2500\u2500
    {
        std::println("\n--- AC1: new helper + facade call ---");
        const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
        const auto bridge_h = read_file("src/compiler/aura_jit_bridge.h");
        const auto hupd = read_file("src/compiler/hot_update_registry.cpp");
        // New helper definition in bridge.cpp.
        CHECK(contains(bridge, "aura_aot_invalidate_owner_slot_for_func_id"),
              "AC1: new helper defined in aura_jit_bridge.cpp");
        // New counter for Agent observability.
        CHECK(contains(bridge, "g_aot_owner_scoped_slot_invalidate_total"),
              "AC1: new counter g_aot_owner_scoped_slot_invalidate_total");
        // Header declaration.
        CHECK(contains(bridge_h, "aura_aot_invalidate_owner_slot_for_func_id"),
              "AC1: helper declared in aura_jit_bridge.h");
        // Call from hard_invalidate_via_facade, gated on epoch didn't move
        // AND multi-eval live > 1.
        const auto hard_inv = hupd.find("hard_invalidate_via_facade");
        const auto hupd_after =
            (hard_inv == std::string::npos) ? std::string{} : hupd.substr(hard_inv);
        CHECK(contains(hupd_after, "aura_aot_invalidate_owner_slot_for_func_id"),
              "AC1: helper called from hard_invalidate_via_facade");
        CHECK(contains(hupd_after, "aura_get_or_preserve_stable_func_id"),
              "AC1: facade looks up stable func_id for the mutated name");
        CHECK(contains(hupd_after, "aura_aot_get_reemit_owner_eval"),
              "AC1: facade resolves reemit owner");
        // The gate: epoch didn't move AND map_size > 1.
        CHECK(contains(hupd_after, "aura_aot_func_table_epoch() == epoch_before"),
              "AC1: epoch-didn't-move gate");
        CHECK(contains(hupd_after, "aura_aot_state_map_size() > 1"), "AC1: multi-eval gate");
        // The helper physically clears fn_ptr + sets soft_stale=1.
        const auto helper_pos = bridge.find("aura_aot_invalidate_owner_slot_for_func_id");
        const auto bridge_after =
            (helper_pos == std::string::npos) ? std::string{} : bridge.substr(helper_pos);
        CHECK(contains(bridge_after, "slot.fn_ptr.store(0, std::memory_order_release)"),
              "AC1: helper zeros fn_ptr (release ordering)");
        CHECK(contains(bridge_after, "slot.soft_stale.store(1, std::memory_order_release)"),
              "AC1: helper sets soft_stale=1");
        CHECK(contains(bridge_after, "g_aot_owner_scoped_slot_invalidate_total.fetch_add(1"),
              "AC1: helper bumps new counter");
        // Owner filter: foreign / unowned slots untouched.
        CHECK(contains(bridge_after, "slot_owner != want_owner"),
              "AC1: owner filter skips foreign / unowned slots");
        // Helper cites #3377 to anchor the regression contract.
        CHECK(contains(bridge_after, "#3377"), "AC1: helper cites #3377");
    }

    // \u2500\u2500 AC2: owner-scope branch (epoch not bumped) + peer-marks still run \u2500\u2500
    {
        std::println("\n--- AC2: epoch not bumped + peer marks preserved ---");
        const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
        // The owner-scoped branch in aura_aot_bump_func_table_epoch does
        // NOT advance g_aot_table_epoch. Verify by checking that
        // g_aot_table_epoch.fetch_add is only called in the non-owner
        // branch (the fall-through global bump).
        const auto bmp_pos = bridge.find("aura_aot_bump_func_table_epoch(void)");
        const auto bridge_after_bmp =
            (bmp_pos == std::string::npos) ? std::string{} : bridge.substr(bmp_pos);
        // Count g_aot_table_epoch.fetch_add occurrences inside this function
        // (one expected, in the global-bump fall-through).
        const auto epoch_add_pos = bridge_after_bmp.find("g_aot_table_epoch.fetch_add");
        CHECK(epoch_add_pos != std::string::npos,
              "AC2: global-bump fall-through advances g_aot_table_epoch");
        // The owner-scoped branch (returns early) is BEFORE this fetch_add.
        const auto os_return_pos = bridge_after_bmp.find("return;");
        CHECK(os_return_pos != std::string::npos,
              "AC2: owner-scoped branch returns early (no epoch add in that path)");
        CHECK(os_return_pos < epoch_add_pos,
              "AC2: owner-scope early-return precedes global-bump fall-through");
        // Peer marks still run in the owner-scoped branch.
        CHECK(contains(bridge_after_bmp.substr(0, epoch_add_pos),
                       "aura_aot_mark_peer_slots_soft_stale(owner)"),
              "AC2: peer AOT soft-stale still runs in owner-scope branch");
        // Peer JIT name soft-stale (#3300) still runs in the facade.
        const auto hupd = read_file("src/compiler/hot_update_registry.cpp");
        CHECK(contains(hupd, "aura_aot_mark_peer_jit_name_soft_stale"),
              "AC2: peer JIT name soft-stale still runs in facade");
    }

    // \u2500\u2500 AC3: Soft / Off / single-eval gate \u2500\u2500
    {
        std::println("\n--- AC3: Soft/Off single-eval gate ---");
        const auto hupd = read_file("src/compiler/hot_update_registry.cpp");
        // hard_invalidate_via_facade early-returns on Soft/Off.
        CHECK(contains(hupd, "aura_production_defaults_active_probe() == 0"),
              "AC3: Soft/Off early-return in hard_invalidate_via_facade");
        CHECK(contains(hupd, "Soft / Off / non-production"), "AC3: Soft/Off branch cited");
        // The new helper call is gated on production + map_size > 1.
        const auto helper_call = hupd.find("aura_aot_invalidate_owner_slot_for_func_id");
        const auto hupd_after =
            (helper_call == std::string::npos) ? std::string{} : hupd.substr(helper_call);
        CHECK(contains(hupd_after, "aura_aot_state_map_size() > 1"),
              "AC3: helper call gated on multi-eval live > 1");
    }

    // \u2500\u2500 AC4: generation-behind predicate unchanged for true epoch bumps \u2500\u2500
    {
        std::println("\n--- AC4: generation-behind predicate unchanged ---");
        const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
        // aot_invalidate_all_stale_slots_for_eval_impl still uses the
        // slot_gen == cur_epoch skip predicate.
        const auto impl_pos = bridge.find("aot_invalidate_all_stale_slots_for_eval_impl");
        const auto bridge_after =
            (impl_pos == std::string::npos) ? std::string{} : bridge.substr(impl_pos);
        CHECK(contains(bridge_after, "slot_gen == cur_epoch"),
              "AC4: generation-behind skip predicate intact");
        // clang-format may split the guard across two lines — accept both
        // the single-line and brace-wrapped forms.
        const bool guard_single = contains(bridge_after, "if (slot_gen == cur_epoch) continue;");
        const bool guard_split =
            contains(bridge_after, "if (slot_gen == cur_epoch)\n            continue;");
        CHECK(guard_single || guard_split, "AC4: skip predicate is the early-continue guard");
    }

    // \u2500\u2500 AC5: no docs/design/3377-*; no test_issue_3377.cpp \u2500\u2500
    {
        std::println("\n--- AC5: no docs/design/3377-*; no test_issue_3377.cpp ---");
        CHECK(read_file("docs/design/3377-owner-scoped-slot-clear.md").empty(),
              "AC5: no docs/design/3377-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_3377.cpp").empty(),
              "AC5: no test_issue_3377.cpp per #81967");
        CHECK(read_file("tests/issues/test_issue_3377.cpp").empty(),
              "AC5: no tests/issues/test_issue_3377.cpp (R1 abandoned scheme)");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_owner_scoped_hard_invalidate_slot_clear();
}
#endif
