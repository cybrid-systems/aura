// tests/core/test_panic_checkpoint_aba.cpp
// Issue #3570: PanicCheckpoint discriminator generation tag — close void* ABA
// across evaluator destroy/realloc (aot:reload / persist:load address reuse).
// #1393 made the discriminator address-only; a recycled address passes the
// check and restore/clear lands on the new occupant. #3570 tags each
// instance with a monotonic generation and snapshots it at save time.
//
//   AC1: gen unchanged → restore runs (behavior preserved for live instance)
//   AC2: same address, gen advanced → restore skipped, clear NOT invoked
//        (original save receiver no longer exists; clearing would drop the
//        new occupant's live checkpoint), gen_aba counter + auto_rollbacks
//   AC3: legacy host (ctx_gen_source == nullptr) → no gen check
//   AC4: Evaluator factory wires gen fields; live instance never trips ABA
//   AC5: source-cite (raii.ixx dtor arm + evaluator.ixx factory wiring)
//   AC6 (#3604): ABA + defer armed at save + new occupant clean →
//        gc_defer drained for the recycled id (drain counted on the
//        issue-sanctioned clears counter), restore/clear still skipped
//   AC7 (#3604): new occupant re-armed a live checkpoint → drain skipped,
//        defer arm intact (#3570 half-graph protection)
//   AC8 (#3604): legacy unwired host (no has_panic_checkpoint probe) →
//        no drain, exact #3570 behavior; steal_complete stays steal-only

#include "test_harness.hpp"

#include "core/gc_hooks.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.core.panic_checkpoint_raii;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::core::panic_cp::g_panic_checkpoint_raii_stats;
using aura::core::panic_cp::PanicCheckpointGuard;
using aura::core::panic_cp::PanicCheckpointHost;
using aura::core::panic_cp::reset_panic_checkpoint_raii_stats;
using aura::test::g_failed;
using aura::test::g_passed;

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

struct FakeHost {
    int saves = 0;
    int restores = 0;
    int clears = 0;

    static bool save_fn(void* p) noexcept {
        ++static_cast<FakeHost*>(p)->saves;
        return true;
    }
    static bool restore_fn(void* p) noexcept {
        ++static_cast<FakeHost*>(p)->restores;
        return true;
    }
    static bool clear_fn(void* p) noexcept {
        ++static_cast<FakeHost*>(p)->clears;
        return true;
    }
};

// Issue #3604: host double mirroring Evaluator::save_panic_checkpoint —
// save arms the per-evaluator gc_defer (keyed by the instance address);
// has_live models the NEW occupant's live-checkpoint state at dtor time.
struct DeferHost {
    bool has_live = false;
    static bool save_fn(void* p) noexcept {
        aura::gc_hooks::arm_gc_defer_pending_panic_for(p);
        return true;
    }
    static bool restore_fn(void*) noexcept { return true; }
    static bool clear_fn(void*) noexcept { return true; }
    static bool has_panic_checkpoint_fn(void* self) noexcept {
        return static_cast<DeferHost*>(self)->has_live;
    }
};

} // namespace

int main() {
    // ── AC1: gen unchanged → restore runs ──
    {
        std::println("\n--- #3570 AC1: gen unchanged → restore runs ---");
        reset_panic_checkpoint_raii_stats();
        FakeHost fake;
        std::atomic<std::uint64_t> gen{5};
        PanicCheckpointHost host{
            &fake, &fake, &FakeHost::save_fn, &FakeHost::restore_fn, &FakeHost::clear_fn, &gen, 5};
        {
            PanicCheckpointGuard g(host);
            CHECK(g.saved(), "AC1: save ok");
        }
        CHECK(fake.restores == 1, "AC1: gen unchanged → restore runs");
        CHECK(fake.clears == 0, "AC1: gen unchanged → no clear");
        CHECK(g_panic_checkpoint_raii_stats.restores_gen_aba_mismatch_total == 0,
              "AC1: aba counter unchanged");
    }

    // ── AC2: same address, gen advanced → ABA skip ──
    {
        std::println("\n--- #3570 AC2: gen advanced → ABA skip ---");
        reset_panic_checkpoint_raii_stats();
        FakeHost fake;
        std::atomic<std::uint64_t> gen{5};
        PanicCheckpointHost host{
            &fake, &fake, &FakeHost::save_fn, &FakeHost::restore_fn, &FakeHost::clear_fn, &gen, 5};
        gen.store(6, std::memory_order_release); // destroy + realloc at same address
        {
            PanicCheckpointGuard g(host);
        }
        CHECK(fake.restores == 0, "AC2: restore skipped on ABA");
        CHECK(fake.clears == 0, "AC2: clear skipped on ABA (new occupant state untouched)");
        CHECK(g_panic_checkpoint_raii_stats.restores_gen_aba_mismatch_total == 1,
              "AC2: gen_aba counter bumps");
        CHECK(g_panic_checkpoint_raii_stats.auto_rollbacks == 1, "AC2: auto_rollbacks counted");
    }

    // ── AC3: legacy host (no gen source) → no gen check ──
    {
        std::println("\n--- #3570 AC3: legacy host skips gen check ---");
        reset_panic_checkpoint_raii_stats();
        FakeHost fake;
        PanicCheckpointHost host{&fake, &fake, &FakeHost::save_fn, &FakeHost::restore_fn,
                                 &FakeHost::clear_fn};
        {
            PanicCheckpointGuard g(host);
        }
        CHECK(fake.restores == 1, "AC3: legacy path restores");
        CHECK(g_panic_checkpoint_raii_stats.restores_gen_aba_mismatch_total == 0,
              "AC3: no aba bump on legacy host");
    }

    // ── AC4: Evaluator factory wiring ──
    {
        std::println("\n--- #3570 AC4: Evaluator factory wires gen ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        PanicCheckpointHost host = Evaluator::panic_checkpoint_host(ev);
        CHECK(host.ctx_gen_source != nullptr, "AC4: factory wires ctx_gen_source");
        CHECK(host.ctx_gen_at_save != 0, "AC4: gen handed out at construction");
        CHECK(*host.ctx_gen_source == host.ctx_gen_at_save,
              "AC4: source matches snapshot for the live instance");
        CHECK(host.has_panic_checkpoint != nullptr, "3604: factory wires has_panic_checkpoint");
        CHECK(!host.has_panic_checkpoint(&ev), "3604: fresh evaluator reports no live checkpoint");
        {
            PanicCheckpointGuard g(host);
        }
        CHECK(g_panic_checkpoint_raii_stats.restores_gen_aba_mismatch_total == 0,
              "AC4: live instance does not trip ABA");
    }

    // ── AC5: source-cite ──
    {
        std::println("\n--- #3570 AC5: source-cite ---");
        const auto raii = read_file("src/core/panic_checkpoint_raii.ixx");
        CHECK(raii.find("restores_gen_aba_mismatch_total") != std::string::npos,
              "AC5: stats field");
        CHECK(raii.find("ctx_gen_source") != std::string::npos, "AC5: host fields");
        CHECK(raii.find("next_instance_discriminator_gen") != std::string::npos, "AC5: gen helper");
        const auto ixx = read_file("src/compiler/evaluator.ixx");
        CHECK(ixx.find("panic_cp_discriminator_gen_") != std::string::npos,
              "AC5: Evaluator member");
        CHECK(ixx.find("panic_cp_discriminator_gen_source()") != std::string::npos,
              "AC5: accessor");
        CHECK(ixx.find("ev.panic_cp_discriminator_gen_source()") != std::string::npos,
              "AC5: factory wiring");
    }

    // ── AC6 (#3604): ABA drains the old occupant's gc_defer ──
    {
        std::println("\n--- #3604 AC6: ABA drains gc_defer for the recycled id ---");
        reset_panic_checkpoint_raii_stats();
        DeferHost fake; // new occupant: no live checkpoint of its own
        std::atomic<std::uint64_t> gen{5};
        PanicCheckpointHost host{&fake,
                                 &fake,
                                 &DeferHost::save_fn,
                                 &DeferHost::restore_fn,
                                 &DeferHost::clear_fn,
                                 &gen,
                                 5,
                                 &DeferHost::has_panic_checkpoint_fn};
        {
            PanicCheckpointGuard g(host); // save arms the per-eval gc_defer
            CHECK(g.saved(), "AC6: save ok");
            CHECK(aura::gc_hooks::gc_deferred_for_evaluator(&fake),
                  "AC6 pre: defer armed under the shared id");
            gen.store(6, std::memory_order_release); // destroy + placement-new
        } // dtor: ABA skip + drain
        CHECK(!aura::gc_hooks::gc_deferred_for_evaluator(&fake),
              "AC6: gc_defer drained — destructive GC no longer stuck on this id");
        CHECK(g_panic_checkpoint_raii_stats.restores_gen_aba_mismatch_total == 1,
              "AC6: gen_aba counter bumps");
        CHECK(g_panic_checkpoint_raii_stats.restores_discriminator_cleared == 1,
              "AC6: drain counted on the sanctioned clears counter");
    }

    // ── AC7 (#3604): new occupant re-armed → drain skipped ──
    {
        std::println("\n--- #3604 AC7: live new-occupant checkpoint keeps its defer arm ---");
        reset_panic_checkpoint_raii_stats();
        DeferHost fake;
        fake.has_live = true; // new occupant holds its own PanicCheckpoint
        std::atomic<std::uint64_t> gen{5};
        PanicCheckpointHost host{&fake,
                                 &fake,
                                 &DeferHost::save_fn,
                                 &DeferHost::restore_fn,
                                 &DeferHost::clear_fn,
                                 &gen,
                                 5,
                                 &DeferHost::has_panic_checkpoint_fn};
        {
            PanicCheckpointGuard g(host);
            CHECK(g.saved(), "AC7: save ok");
            gen.store(6, std::memory_order_release);
        }
        CHECK(aura::gc_hooks::gc_deferred_for_evaluator(&fake),
              "AC7: new occupant's defer arm intact (no half-graph exposure)");
        CHECK(g_panic_checkpoint_raii_stats.restores_discriminator_cleared == 0,
              "AC7: no drain counted");
        // Cleanup: drop the modelled occupant arm so process-wide depth
        // stays balanced for the rest of the batch.
        (void)aura::gc_hooks::clear_gc_defer_for_evaluator(&fake);
        (void)aura::gc_hooks::reconcile_gc_defer_bits_after_clear();
    }

    // ── AC8 (#3604): legacy unwired host → no drain (#3570 exact) ──
    {
        std::println("\n--- #3604 AC8: unwired host keeps #3570 behavior ---");
        reset_panic_checkpoint_raii_stats();
        DeferHost fake;
        std::atomic<std::uint64_t> gen{5};
        PanicCheckpointHost host{
            &fake, &fake, &DeferHost::save_fn, &DeferHost::restore_fn, &DeferHost::clear_fn,
            &gen,  5};
        {
            PanicCheckpointGuard g(host);
            CHECK(g.saved(), "AC8: save ok");
            CHECK(aura::gc_hooks::gc_deferred_for_evaluator(&fake), "AC8 pre: defer armed");
            gen.store(6, std::memory_order_release);
        }
        CHECK(aura::gc_hooks::gc_deferred_for_evaluator(&fake),
              "AC8: no probe wired → drain skipped (legacy exact)");
        CHECK(g_panic_checkpoint_raii_stats.restores_discriminator_cleared == 0,
              "AC8: no drain counted");
        (void)aura::gc_hooks::clear_gc_defer_for_evaluator(&fake);
        (void)aura::gc_hooks::reconcile_gc_defer_bits_after_clear();
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
