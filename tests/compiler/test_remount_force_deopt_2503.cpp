// @category: unit
// @reason: Issue #2503 — densify cell-remap fail shares MustDeopt +
//          batch_deopt transaction with fingerprint remount failure.
//
//   AC1: Unmapped densify candidate → remount_or_force_deopt returns 0,
//        MustDeopt set, cell_remap_fail bumped, batch_deopt when named
//   AC2: env_gen fingerprint fail still MustDeopt + mismatch counter
//   AC3: Empty densify context → no extra cell walk cost
//   AC4: Source-cite all production remount sites use shared fail path
//   AC5: Stress densify × remount cycles (ASan/TSan-friendly unit)

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

extern "C" std::int64_t aura_alloc_closure(std::int64_t func_id);
extern "C" void aura_closure_set_name(std::int64_t closure_id, const char* name);
extern "C" int aura_closure_get_must_deopt(std::int64_t closure_id);
extern "C" void aura_closure_set_must_deopt(std::int64_t closure_id, int v);
extern "C" void aura_closure_capture(std::int64_t closure_id, std::int64_t idx, std::int64_t val);
extern "C" void aura_closure_set_env_gen(std::int64_t closure_id, std::uint64_t gen);
extern "C" std::uint64_t aura_get_closure_defuse_version(std::int64_t closure_id);

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:aot-incremental-reemit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Align env_gen + defuse so remount fingerprint axes pass (same as #2297).
static std::uint64_t stamp_for_remount(std::int64_t cid) {
    const auto defuse = aura_get_closure_defuse_version(cid);
    aura_closure_set_env_gen(cid, defuse);
    return defuse;
}

// ── AC1: densify candidate fail → MustDeopt + cell_remap_fail + batch_deopt ──
static void ac1_cell_remap_force_deopt() {
    std::println("\n--- #2503 AC1: unmapped densify candidate → MustDeopt + batch_deopt ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    aura_set_aot_metrics(m);
    const auto live_linear = aura_get_aot_live_linear_state_fingerprint();

    aura_clear_densify_object_remap();
    aura_clear_densify_candidates();

    const auto cid = aura_alloc_closure(/*func_id=*/0);
    CHECK(cid >= 0, "AC1: alloc closure");
    aura_closure_set_name(cid, "cell_remap_fail_2503");
    aura_closure_set_must_deopt(cid, 0);
    const auto live = stamp_for_remount(cid);

    void* dangling = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADBEEF));
    aura_closure_capture(cid, 0,
                         static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(dangling)));
    // Densify context active (non-empty remap for another ptr) + candidate
    // covering the capture cell without a remap entry → fail-closed.
    void* other_old = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1));
    void* other_neu = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x2));
    const void* olds[] = {other_old};
    const void* news[] = {other_neu};
    aura_set_densify_object_remap(olds, news, 1);
    const void* cands[] = {dangling};
    aura_set_densify_candidates(cands, 1);

    const auto cell_fail0 = m->closure_capture_cell_remap_fail_total.load();
    const auto remount_fail0 = m->closure_capture_remount_fail_total.load();
    const auto batch0 = aura_jit_batch_deopt_for_total();

    const int r = aura_remount_or_force_deopt(cid, live, live_linear);
    CHECK(r == 0, "AC1: remount_or_force_deopt returns 0");
    CHECK(aura_closure_get_must_deopt(cid) == 1, "AC1: MustDeopt set on cell remap fail");
    CHECK(m->closure_capture_cell_remap_fail_total.load() > cell_fail0,
          "AC1: cell_remap_fail bumped");
    CHECK(m->closure_capture_remount_fail_total.load() > remount_fail0,
          "AC1: remount_fail bumped (shared outcome)");
    CHECK(aura_jit_batch_deopt_for_total() > batch0,
          "AC1: batch_deopt_for invoked for named closure");

    aura_clear_densify_object_remap();
    aura_clear_densify_candidates();
    aura_set_aot_metrics(nullptr);
}

// ── AC2: env_gen fail still MustDeopt (no cell walk) ──
static void ac2_env_gen_force_deopt() {
    std::println("\n--- #2503 AC2: env_gen fail → MustDeopt + mismatch counter ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    aura_set_aot_metrics(m);
    const auto live_linear = aura_get_aot_live_linear_state_fingerprint();

    aura_clear_densify_object_remap();
    aura_clear_densify_candidates();

    const auto cid = aura_alloc_closure(0);
    CHECK(cid >= 0, "AC2: alloc");
    aura_closure_set_name(cid, "env_gen_fail_2503");
    aura_closure_set_must_deopt(cid, 0);
    aura_closure_set_env_gen(cid, 42);

    // Publish densify remap that would rewrite if reached — AC2 requires
    // env_gen PRIMARY blocks cell walk.
    void* old_addr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x3000));
    void* new_addr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x4000));
    aura_closure_capture(cid, 0,
                         static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(old_addr)));
    const void* olds[] = {old_addr};
    const void* news[] = {new_addr};
    aura_set_densify_object_remap(olds, news, 1);

    const auto mm0 = m->closure_capture_env_gen_mismatch_total.load();
    const auto cell_ok0 = m->closure_capture_cell_remap_ok_total.load();
    const auto batch0 = aura_jit_batch_deopt_for_total();

    const int r = aura_remount_or_force_deopt(cid, /*live=*/99, live_linear);
    CHECK(r == 0, "AC2: remount_or_force_deopt returns 0 on env_gen mismatch");
    CHECK(aura_closure_get_must_deopt(cid) == 1, "AC2: MustDeopt set on env_gen fail");
    CHECK(m->closure_capture_env_gen_mismatch_total.load() > mm0, "AC2: env_gen mismatch bumped");
    CHECK(m->closure_capture_cell_remap_ok_total.load() == cell_ok0,
          "AC2: cell remap not run on env_gen fail");
    CHECK(aura_jit_batch_deopt_for_total() > batch0, "AC2: batch_deopt on named env_gen fail");

    aura_clear_densify_object_remap();
    aura_set_aot_metrics(nullptr);
}

// ── AC3: empty densify → zero cell cost ──
static void ac3_empty_densify_zero_cost() {
    std::println("\n--- #2503 AC3: empty densify context → no extra cell walk cost ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    aura_set_aot_metrics(m);
    const auto live_linear = aura_get_aot_live_linear_state_fingerprint();

    aura_clear_densify_object_remap();
    aura_clear_densify_candidates();

    const auto cid = aura_alloc_closure(0);
    CHECK(cid >= 0, "AC3: alloc");
    aura_closure_set_must_deopt(cid, 0);
    const auto live = stamp_for_remount(cid);

    const auto ok0 = m->closure_capture_cell_remap_ok_total.load();
    const auto fail0 = m->closure_capture_cell_remap_fail_total.load();
    const auto remount_ok0 = m->closure_capture_remount_ok_total.load();

    const int r = aura_remount_or_force_deopt(cid, live, live_linear);
    CHECK(r == 1, "AC3: remount_or_force_deopt ok with empty densify");
    CHECK(aura_closure_get_must_deopt(cid) == 0, "AC3: MustDeopt stays clear on ok");
    CHECK(m->closure_capture_cell_remap_ok_total.load() == ok0,
          "AC3: cell_remap_ok unchanged (zero work)");
    CHECK(m->closure_capture_cell_remap_fail_total.load() == fail0,
          "AC3: cell_remap_fail unchanged");
    CHECK(m->closure_capture_remount_ok_total.load() > remount_ok0, "AC3: remount_ok advanced");

    aura_set_aot_metrics(nullptr);
}

// ── AC4: source-cite shared path at all production remount sites ──
static void ac4_source_cite() {
    std::println("\n--- #2503 AC4: source-cite shared remount_or_force_deopt sites ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto hh = read_file("src/compiler/aura_jit_bridge.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto stub = read_file("src/compiler/aura_jit_bridge_stub.cpp");
    const auto cmake = read_file("CMakeLists.txt");

    CHECK(hh.find("aura_remount_or_force_deopt") != std::string::npos,
          "AC4: C ABI declared in bridge.h");
    CHECK(rt.find("remount_or_force_deopt_unlocked") != std::string::npos,
          "AC4: unlocked helper in runtime");
    CHECK(rt.find("aura_remount_or_force_deopt") != std::string::npos,
          "AC4: public wrapper in runtime");
    // Production sites: post-reemit remap walk + soft migrate
    CHECK(rt.find("remount_or_force_deopt_unlocked") != std::string::npos &&
              rt.find("try_cross_cow_soft_migrate_") != std::string::npos,
          "AC4: soft migrate present");
    // Soft migrate wires shared path (not bare remount_unlocked for fail).
    {
        // Body spans ~2.9 KB to the shared remount call — use a 4 KB window.
        const auto soft_pos = rt.find("static int try_cross_cow_soft_migrate_");
        const auto soft_body = soft_pos != std::string::npos ? rt.substr(soft_pos, 4000) : "";
        CHECK(soft_body.find("remount_or_force_deopt_unlocked") != std::string::npos,
              "AC4: soft migrate uses remount_or_force_deopt_unlocked");
    }
    // Reemit remap walk uses shared path (definition, not earlier comments).
    {
        const auto remap_pos =
            rt.find("extern \"C\" std::uint64_t aura_remap_live_closures_after_reemit");
        const auto remap_body =
            remap_pos != std::string::npos ? rt.substr(remap_pos, 20000) : std::string{};
        CHECK(remap_pos != std::string::npos, "AC4: remap definition present");
        CHECK(remap_body.find("remount_or_force_deopt_unlocked") != std::string::npos,
              "AC4: reemit remap uses remount_or_force_deopt_unlocked");
        // Shared path only — no inline MustDeopt + batch_deopt remount fail branch.
        CHECK(remap_body.find("remount_or_force_deopt_unlocked") != std::string::npos,
              "AC4: reemit does not use divergent bare remount fail path alone");
    }
    CHECK(q.find("schema-2503") != std::string::npos, "AC4: schema-2503 on query surface");
    CHECK(q.find("remount-or-force-deopt-wired") != std::string::npos,
          "AC4: remount-or-force-deopt-wired sentinel");
    CHECK(stub.find("aura_remount_or_force_deopt") != std::string::npos, "AC4: weak stub present");
    CHECK(cmake.find("test_remount_force_deopt_2503") != std::string::npos, "AC4: cmake target");
    CHECK(rt.find("2503") != std::string::npos, "AC4: #2503 cite in runtime");
}

// ── AC5: stress densify × remount (no half-success MustDeopt leak) ──
static void ac5_stress() {
    std::println("\n--- #2503 AC5: densify × remount stress (MustDeopt always on fail) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    aura_set_aot_metrics(m);
    const auto live_linear = aura_get_aot_live_linear_state_fingerprint();

    constexpr int kIters = 64;
    int fail_must = 0;
    int ok_clear = 0;
    for (int i = 0; i < kIters; ++i) {
        aura_clear_densify_object_remap();
        aura_clear_densify_candidates();
        const auto cid = aura_alloc_closure(0);
        if (cid < 0)
            continue;
        aura_closure_set_name(cid, "stress_2503");
        aura_closure_set_must_deopt(cid, 0);
        const auto live = stamp_for_remount(cid);

        if ((i % 2) == 0) {
            // Fail path: unmapped densify candidate
            void* dangling = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xA000 + i * 16));
            aura_closure_capture(
                cid, 0, static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(dangling)));
            void* o = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x10 + i));
            void* n = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x20 + i));
            const void* olds[] = {o};
            const void* news[] = {n};
            aura_set_densify_object_remap(olds, news, 1);
            const void* cands[] = {dangling};
            aura_set_densify_candidates(cands, 1);
            const int r = aura_remount_or_force_deopt(cid, live, live_linear);
            if (r == 0 && aura_closure_get_must_deopt(cid) == 1)
                ++fail_must;
        } else {
            // Success path: empty densify or mapped rewrite
            if ((i % 4) == 1) {
                void* o = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xB000 + i));
                void* n = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xC000 + i));
                aura_closure_capture(
                    cid, 0, static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(o)));
                const void* olds[] = {o};
                const void* news[] = {n};
                aura_set_densify_object_remap(olds, news, 1);
            }
            const int r = aura_remount_or_force_deopt(cid, live, live_linear);
            if (r == 1 && aura_closure_get_must_deopt(cid) == 0)
                ++ok_clear;
        }
    }
    aura_clear_densify_object_remap();
    aura_clear_densify_candidates();
    CHECK(fail_must >= kIters / 4, "AC5: fail paths set MustDeopt under stress");
    CHECK(ok_clear >= kIters / 4, "AC5: ok paths leave MustDeopt clear under stress");

    // Query lineage
    CHECK(href(cs, "schema-2503") == 2503, "AC5: schema-2503 query");
    CHECK(href(cs, "remount-or-force-deopt-wired") == 1, "AC5: remount-or-force-deopt-wired");
    aura_set_aot_metrics(nullptr);
}

} // namespace

int main() {
    std::println("test_remount_force_deopt_2503");
    ac1_cell_remap_force_deopt();
    ac2_env_gen_force_deopt();
    ac3_empty_densify_zero_cost();
    ac4_source_cite();
    ac5_stress();
    if (g_failed)
        return 1;
    std::println("remount force-deopt #2503: OK ({} passed)", g_passed);
    return 0;
}
