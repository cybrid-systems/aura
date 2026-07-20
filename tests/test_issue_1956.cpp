// @category: unit
// @reason: Issue #1956 — HotUpdateRegistry unified coordination center
// for re-emit / region / epoch / dirty listeners + metrics.
//
//   AC1: HotUpdateRegistry class + query:hot-update-registry-stats schema-1956
//   AC2: C ABI setters bump registry register_calls / wiring flags
//   AC3: dynamic epoch + dirty listeners fire on notify
//   AC4: reemit pipeline aggregates on aura_reemit_aot_for_dirty
//   AC5: stable func_id preserve/assign tracked
//   AC6: multi-round stress; schema holds; docs cite #1956

#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"
#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::hot_update_registry;
using aura::compiler::HotUpdateRegistry;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:hot-update-registry-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

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

struct ReemitFeed {
    std::vector<const char*> names;
    std::size_t i = 0;
};

static bool reemit_iter(void* userdata, const char** out_name, std::uint64_t* out_region,
                        bool* out_from_cc) {
    auto* f = static_cast<ReemitFeed*>(userdata);
    if (f->i >= f->names.size())
        return false;
    *out_name = f->names[f->i++];
    *out_region = 0;
    *out_from_cc = false;
    return true;
}

static bool emit_ok(const char* /*name*/, std::uint64_t /*region*/, void* /*ud*/) {
    return true;
}

static void ac1_schema() {
    std::println("\n--- AC1: schema-1956 surface ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:hot-update-registry-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1956, "schema 1956");
    CHECK(href(cs, "schema-1956") == 1956, "schema-1956");
    CHECK(href(cs, "issue-1956") == 1956, "issue-1956");
    CHECK(href(cs, "active") == 1, "active");
    CHECK(href(cs, "registry-class-wired") == 1, "registry class");
    CHECK(href(cs, "mvp-single-workspace") == 1, "MVP scope");
    CHECK(href(cs, "hot_update_registry_register_calls_total") >= 0, "register metric");
}

static void ac2_abi_wiring() {
    std::println("\n--- AC2: C ABI setters update registry ---");
    auto& reg = hot_update_registry();
    const auto r0 = reg.register_calls_total();
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_fn(nullptr, nullptr);
    aura_set_is_define_dirty_fn(nullptr, nullptr);
    aura_set_aot_emit_region_mask(0x3);
    CHECK(reg.register_calls_total() > r0, "register_calls grew");
    auto snap = reg.snapshot();
    CHECK(snap.emit_region_mask == 0x3, "region mask stored");
    CHECK(snap.reemit_provider_wired == 0, "reemit unwired");
    // Wire then unwire
    ReemitFeed f{{"f"}};
    aura_set_reemit_candidate_fn(&reemit_iter, &f);
    CHECK(reg.snapshot().reemit_provider_wired == 1, "reemit wired");
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    CHECK(reg.snapshot().reemit_provider_wired == 0, "reemit clear");
}

static void ac3_listeners() {
    std::println("\n--- AC3: dynamic epoch/dirty listeners ---");
    auto& reg = hot_update_registry();
    reg.clear_listeners();
    std::atomic<int> epochs{0};
    std::atomic<int> dirties{0};
    std::string last_dirty;
    reg.register_epoch_listener([&](std::uint64_t e) {
        epochs.fetch_add(1);
        (void)e;
    });
    reg.register_dirty_listener([&](const char* name) {
        dirties.fetch_add(1);
        if (name)
            last_dirty = name;
    });
    const auto e0 = reg.epoch_notify_total();
    const auto d0 = reg.dirty_notify_total();
    reg.notify_epoch_bump(42);
    reg.notify_dirty_define("foo");
    CHECK(epochs.load() == 1, "epoch listener fired");
    CHECK(dirties.load() == 1, "dirty listener fired");
    CHECK(last_dirty == "foo", "dirty name");
    CHECK(reg.epoch_notify_total() == e0 + 1, "epoch notify metric");
    CHECK(reg.dirty_notify_total() == d0 + 1, "dirty notify metric");
    reg.clear_listeners();
    CHECK(reg.snapshot().epoch_listeners == 0, "cleared");
}

static void ac4_reemit_pipeline() {
    std::println("\n--- AC4: reemit pipeline aggregates ---");
    auto& reg = hot_update_registry();
    aura_clear_stable_func_id_map();
    ReemitFeed f{{"alpha", "beta"}};
    aura_set_reemit_candidate_fn(&reemit_iter, &f);
    aura_set_aot_emit_fn(&emit_ok, nullptr);
    const auto p0 = reg.snapshot().reemit_pipeline_calls_total;
    const auto c0 = reg.snapshot().reemit_candidates_total;
    const auto s0 = reg.snapshot().reemit_success_total;
    const auto n = aura_reemit_aot_for_dirty(1);
    // Return is success count when emit fn wired; last-call stat is the ground truth.
    CHECK(n >= 1 || aura_reemit_success_count() >= 1, "reemit progressed");
    CHECK(aura_reemit_success_count() == 2 || n == 2, "2 successes");
    auto snap = reg.snapshot();
    CHECK(snap.reemit_pipeline_calls_total == p0 + 1, "pipeline +1");
    CHECK(snap.reemit_candidates_total >= c0 + 2, "candidates +2");
    CHECK(snap.reemit_success_total >= s0 + 2, "success +2");
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_fn(nullptr, nullptr);
}

static void ac5_stable_id() {
    std::println("\n--- AC5: stable func_id tracking ---");
    auto& reg = hot_update_registry();
    aura_clear_stable_func_id_map();
    const auto a0 = reg.snapshot().stable_id_assign_total;
    const auto p0 = reg.snapshot().stable_id_preserve_total;
    int preserved = 0;
    const auto id1 = aura_get_or_preserve_stable_func_id("gamma", &preserved);
    CHECK(id1 != 0, "assigned id");
    CHECK(preserved == 0, "first assign");
    CHECK(reg.snapshot().stable_id_assign_total == a0 + 1, "assign +1");
    const auto id2 = aura_get_or_preserve_stable_func_id("gamma", &preserved);
    CHECK(id2 == id1, "same id");
    CHECK(preserved == 1, "preserved");
    CHECK(reg.snapshot().stable_id_preserve_total == p0 + 1, "preserve +1");
    CHECK(reg.snapshot().stable_func_id_map_size >= 1, "map size");
}

static void ac6_stress_and_docs() {
    std::println("\n--- AC6: stress + docs ---");
    auto& reg = hot_update_registry();
    reg.clear_listeners();
    std::atomic<int> hits{0};
    reg.register_epoch_listener([&](std::uint64_t) { hits.fetch_add(1); });
    for (int i = 0; i < 100; ++i) {
        reg.notify_epoch_bump(static_cast<std::uint64_t>(i));
        reg.notify_dirty_define("stress");
    }
    CHECK(hits.load() == 100, "100 epoch notifies");
    CompilerService cs;
    CHECK(href(cs, "schema-1956") == 1956, "schema after stress");
    auto doc = read_first({"docs/hot-update.md", "../docs/hot-update.md"});
    CHECK(!doc.empty() && doc.find("1956") != std::string::npos, "docs cite #1956");
    auto hh = read_first(
        {"src/compiler/hot_update_registry.hh", "../src/compiler/hot_update_registry.hh"});
    CHECK(!hh.empty() && hh.find("HotUpdateRegistry") != std::string::npos, "header class");
    auto obs = read_first({"src/compiler/evaluator_primitives_observability.cpp",
                           "../src/compiler/evaluator_primitives_observability.cpp"});
    CHECK(!obs.empty() && obs.find("query:hot-update-registry-stats") != std::string::npos,
          "obs catalog lists query");
    auto mut = read_first({"src/compiler/evaluator_primitives_mutate.cpp",
                           "../src/compiler/evaluator_primitives_mutate.cpp"});
    CHECK(!mut.empty() && mut.find("hot-update-registry-stats") != std::string::npos,
          "mutate registers query");
    reg.clear_listeners();
}

} // namespace

int main() {
    std::println("=== Issue #1956: HotUpdateRegistry coordination center ===");
    ac1_schema();
    ac2_abi_wiring();
    ac3_listeners();
    ac4_reemit_pipeline();
    ac5_stable_id();
    ac6_stress_and_docs();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
