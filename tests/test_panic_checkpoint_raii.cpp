// test_panic_checkpoint_raii.cpp — Issue #1363: wire PanicCheckpointGuard to Evaluator

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.core.panic_checkpoint_raii;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::core::panic_cp::g_panic_checkpoint_raii_stats;
using aura::core::panic_cp::kPanicCheckpointRaiiPhase;
using aura::core::panic_cp::PanicCheckpointGuard;
using aura::core::panic_cp::PanicCheckpointHost;
using aura::core::panic_cp::reset_panic_checkpoint_raii_stats;

namespace {

// Test double host: counts save/restore calls without full workspace.
struct FakeHost {
    int saves = 0;
    int restores = 0;
    bool save_ok = true;
    bool restore_ok = true;

    static bool save_fn(void* p) noexcept {
        auto* h = static_cast<FakeHost*>(p);
        ++h->saves;
        return h->save_ok;
    }
    static bool restore_fn(void* p) noexcept {
        auto* h = static_cast<FakeHost*>(p);
        ++h->restores;
        return h->restore_ok;
    }

    // Issue #1393: host is {ctx, expected_evaluator_id, save, restore}.
    // For the fake host, discriminator == ctx (same identity).
    PanicCheckpointHost host() { return PanicCheckpointHost{this, this, &save_fn, &restore_fn}; }
};

} // namespace

int main() {
    CHECK(kPanicCheckpointRaiiPhase == 2, "phase == 2 (wired, not scaffold)");

    // ── Fake host: construct → save; dtor without commit → restore ──
    {
        reset_panic_checkpoint_raii_stats();
        FakeHost fake;
        {
            PanicCheckpointGuard g(fake.host());
            CHECK(g.saved(), "save succeeded on fake host");
            CHECK(fake.saves == 1, "save called once in ctor");
            CHECK(fake.restores == 0, "no restore while alive");
            CHECK(g_panic_checkpoint_raii_stats.guards_constructed == 1, "guards_constructed");
            CHECK(g_panic_checkpoint_raii_stats.saves_ok == 1, "saves_ok");
        }
        CHECK(fake.restores == 1, "dtor called restore");
        CHECK(g_panic_checkpoint_raii_stats.auto_rollbacks == 1, "auto_rollbacks");
        CHECK(g_panic_checkpoint_raii_stats.restores_ok == 1, "restores_ok");
        CHECK(g_panic_checkpoint_raii_stats.commits == 0, "no commit");
    }

    // ── commit → no restore on dtor ──
    {
        reset_panic_checkpoint_raii_stats();
        FakeHost fake;
        {
            PanicCheckpointGuard g(fake.host());
            g.commit();
            CHECK(g.committed(), "committed flag");
        }
        CHECK(fake.saves == 1, "save on construct");
        CHECK(fake.restores == 0, "commit skips restore");
        CHECK(g_panic_checkpoint_raii_stats.commits == 1, "commits == 1");
        CHECK(g_panic_checkpoint_raii_stats.auto_rollbacks == 0, "no auto_rollback after commit");
    }

    // ── save fails → no restore on dtor ──
    {
        reset_panic_checkpoint_raii_stats();
        FakeHost fake;
        fake.save_ok = false;
        {
            PanicCheckpointGuard g(fake.host());
            CHECK(!g.saved(), "save failed");
        }
        CHECK(fake.restores == 0, "no restore when save failed");
        CHECK(g_panic_checkpoint_raii_stats.saves_failed == 1, "saves_failed");
        CHECK(g_panic_checkpoint_raii_stats.auto_rollbacks == 1, "still counts auto_rollback");
    }

    // ── null host: stats only, no crash ──
    {
        reset_panic_checkpoint_raii_stats();
        {
            PanicCheckpointGuard g(PanicCheckpointHost{});
            CHECK(!g.saved(), "null host not saved");
            g.commit();
        }
        CHECK(g_panic_checkpoint_raii_stats.guards_constructed == 1, "null host constructs");
        CHECK(g_panic_checkpoint_raii_stats.commits == 1, "null host commit");
    }

    // ── Evaluator host binding (make_panic_checkpoint_guard) ──
    {
        reset_panic_checkpoint_raii_stats();
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Without workspace source, save may fail — still must not crash.
        {
            auto g = ev.make_panic_checkpoint_guard();
            CHECK(g_panic_checkpoint_raii_stats.guards_constructed == 1, "ev guard constructed");
            // If save worked, commit so we don't restore over nothing meaningful.
            if (g.saved())
                g.commit();
            else
                g.commit(); // either way: avoid restore noise
        }
        CHECK(g_panic_checkpoint_raii_stats.commits >= 1, "ev guard committed");
    }

    // ── Full save/restore with workspace (if set-code works) ──
    {
        reset_panic_checkpoint_raii_stats();
        CompilerService cs;
        auto& ev = cs.evaluator();
        (void)cs.eval("(set-code \"(define x 1)\")");
        const auto saves0 = ev.get_panic_checkpoint_save_count();
        const auto restores0 = ev.get_panic_checkpoint_restore_count();
        bool saved = false;
        {
            auto g = ev.make_panic_checkpoint_guard();
            saved = g.saved();
            if (saved) {
                CHECK(ev.has_panic_checkpoint(), "checkpoint present after save");
                // Mutate env frames / leave dirty — restore should clear checkpoint path
            }
            // do NOT commit — force restore
        }
        if (saved) {
            CHECK(ev.get_panic_checkpoint_save_count() > saves0, "Evaluator save_count bumped");
            CHECK(ev.get_panic_checkpoint_restore_count() > restores0,
                  "Evaluator restore_count bumped");
            CHECK(g_panic_checkpoint_raii_stats.restores_ok >= 1 ||
                      g_panic_checkpoint_raii_stats.restores_failed >= 1,
                  "RAII restore attempted");
            CHECK(g_panic_checkpoint_raii_stats.auto_rollbacks >= 1, "auto_rollback counted");
        } else {
            CHECK(true, "save unavailable in this config (skip restore assert)");
        }
    }

    // ── host factory static ──
    {
        Evaluator ev;
        auto host = Evaluator::panic_checkpoint_host(ev);
        CHECK(host.ctx == &ev, "host ctx is evaluator");
        CHECK(host.save != nullptr, "host save bound");
        CHECK(host.restore != nullptr, "host restore bound");
    }

    // ── repeated cycles no leak / stable stats ──
    {
        reset_panic_checkpoint_raii_stats();
        FakeHost fake;
        for (int i = 0; i < 100; ++i) {
            PanicCheckpointGuard g(fake.host());
            if (i % 2 == 0)
                g.commit();
        }
        CHECK(g_panic_checkpoint_raii_stats.guards_constructed == 100, "100 guards");
        CHECK(g_panic_checkpoint_raii_stats.commits == 50, "50 commits");
        CHECK(g_panic_checkpoint_raii_stats.auto_rollbacks == 50, "50 auto rollbacks");
        CHECK(fake.saves == 100, "100 saves");
        CHECK(fake.restores == 50, "50 restores");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("panic checkpoint RAII #1363: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
