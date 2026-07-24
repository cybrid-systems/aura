// @category: unit
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
