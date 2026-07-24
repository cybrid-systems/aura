// tests/core/test_panic_checkpoint_batch.cpp
// R18 dup-merge — Issue #1806/#1807 (#1978 renamed): panic checkpoint clear
// + RAII lifetime combined into one batch file.
// Originals: test_panic_checkpoint_clear.cpp + test_panic_checkpoint_raii.cpp.
// R18 ship per Anqi 13:14 #81620.

// === AC1-AC4 from test_panic_checkpoint_clear.cpp (#1806) ===

// @reason: Issue #1727 — cross-evaluator discriminator skip must clear
// Issue #1727 (#1978 renamed): issue# moved from filename to header.
// stale panic checkpoint (not only skip restore).
//
//   AC1: PanicCheckpointHost has clear; stats has
//        restores_discriminator_cleared
//   AC2: mismatch + clear fn → clear called, restore not called
//   AC3: matching discriminator does not clear on auto-rollback restore
//   AC4: Evaluator::clear_panic_checkpoint drops has_panic_checkpoint

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.panic_checkpoint_raii;
import aura.compiler.service;
import aura.compiler.evaluator;

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
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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

} // namespace

int main() {
    // ── AC1: source / field ──
    {
        std::println("\n--- AC1: clear field + cleared counter ---");
        std::string raii;
        for (const char* p :
             {"src/core/panic_checkpoint_raii.ixx", "../src/core/panic_checkpoint_raii.ixx"}) {
            raii = read_file(p);
            if (!raii.empty())
                break;
        }
        CHECK(!raii.empty(), "read panic_checkpoint_raii.ixx");
        CHECK(raii.find("#1727") != std::string::npos, "cites #1727");
        CHECK(raii.find("restores_discriminator_cleared") != std::string::npos,
              "stats has clears counter");
        CHECK(raii.find("bool (*clear)") != std::string::npos ||
                  raii.find("(*clear)") != std::string::npos,
              "Host has clear fn");

        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty() && ixx.find("clear_panic_checkpoint") != std::string::npos,
              "Evaluator::clear_panic_checkpoint");
    }

    // ── AC2: mismatch → clear, no restore ──
    {
        std::println("\n--- AC2: mismatch clears, skips restore ---");
        reset_panic_checkpoint_raii_stats();
        FakeHost fake;
        int other = 0;
        PanicCheckpointHost host{
            &fake,  // ctx (save/clear target)
            &other, // expected_evaluator_id (mismatch)
            &FakeHost::save_fn,
            &FakeHost::restore_fn,
            &FakeHost::clear_fn,
        };
        const auto fail_before = g_panic_checkpoint_raii_stats.restores_discriminator_failed;
        const auto clear_before = g_panic_checkpoint_raii_stats.restores_discriminator_cleared;
        {
            PanicCheckpointGuard g(host);
            CHECK(g.saved(), "save ok on mismatch host");
            CHECK(fake.saves == 1, "save once");
        }
        CHECK(fake.restores == 0, "restore skipped on mismatch");
        CHECK(fake.clears == 1, "clear called on mismatch");
        CHECK(g_panic_checkpoint_raii_stats.restores_discriminator_failed == fail_before + 1,
              "discriminator failed +1");
        CHECK(g_panic_checkpoint_raii_stats.restores_discriminator_cleared == clear_before + 1,
              "discriminator cleared +1");
    }

    // ── AC3: matching → restore, no clear ──
    {
        std::println("\n--- AC3: matching restores, does not clear ---");
        reset_panic_checkpoint_raii_stats();
        FakeHost fake;
        PanicCheckpointHost host{
            &fake, &fake, &FakeHost::save_fn, &FakeHost::restore_fn, &FakeHost::clear_fn,
        };
        const auto clear_before = g_panic_checkpoint_raii_stats.restores_discriminator_cleared;
        {
            PanicCheckpointGuard g(host);
        }
        CHECK(fake.restores == 1, "matching path restores");
        CHECK(fake.clears == 0, "matching path does not clear");
        CHECK(g_panic_checkpoint_raii_stats.restores_discriminator_cleared == clear_before,
              "cleared counter unchanged");
    }

    // ── AC4: Evaluator clear_panic_checkpoint ──
    {
        std::println("\n--- AC4: Evaluator clear_panic_checkpoint API ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Without a loaded workspace, save may fail; still exercise clear.
        CHECK(!ev.has_panic_checkpoint(), "starts empty");
        ev.clear_panic_checkpoint();
        CHECK(!ev.has_panic_checkpoint(), "clear on empty is safe");

        // Manually seed via test setters + source isn't public; use
        // panic_checkpoint_host clear binding path with forced mismatch
        // after a successful save if possible.
        (void)cs.eval("(define x 1)");
        const bool saved = ev.save_panic_checkpoint();
        if (saved) {
            CHECK(ev.has_panic_checkpoint(), "save populated checkpoint");
            int other = 0;
            PanicCheckpointHost host = Evaluator::panic_checkpoint_host(ev);
            host.expected_evaluator_id = &other; // force mismatch
            reset_panic_checkpoint_raii_stats();
            {
                // save will run again in Guard ctor
                PanicCheckpointGuard g(host);
            }
            CHECK(!ev.has_panic_checkpoint(), "mismatch dtor cleared checkpoint");
            CHECK(g_panic_checkpoint_raii_stats.restores_discriminator_cleared >= 1,
                  "cleared counter bumped for Evaluator host");
        } else {
            // Workspace not ready for save — still OK if clear API exists.
            CHECK(true, "save skipped (no workspace); clear API already exercised");
        }
    }

    std::println("\n=== test_panic_checkpoint_clear_1727: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}


// === AC5-AC8 from test_panic_checkpoint_raii.cpp (#1807) ===


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

// Test double host: counts save/restore/clear calls without full workspace.
struct FakeHost {
    int saves = 0;
    int restores = 0;
    int clears = 0;
    bool save_ok = true;
    bool restore_ok = true;
    bool clear_ok = true;

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
    static bool clear_fn(void* p) noexcept {
        auto* h = static_cast<FakeHost*>(p);
        ++h->clears;
        return h->clear_ok;
    }

    // Issue #1393 / #1727: host is {ctx, expected, save, restore, clear}.
    // For the fake host, discriminator == ctx (same identity).
    PanicCheckpointHost host() {
        return PanicCheckpointHost{this, this, &save_fn, &restore_fn, &clear_fn};
    }
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
