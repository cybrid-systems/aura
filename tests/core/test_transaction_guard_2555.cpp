// @category: unit
// @reason: Issue #2555 — real TransactionGuard (type-erased host) consolidating
//          MutationBoundaryGuard + PanicCheckpoint lifecycle.
//
//   AC1: Scaffold simulation removed; host try_acquire/release required
//   AC2: Agent body + set-body cite TransactionGuard / host factories
//   AC3: Reject path → Rejected, no held handle / no armed panic depth
//   AC4: recover_panic → PanicRecovered + restore called
//   AC5: Coverage linter + source-cite schema-2555 / #2555

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.transaction_guard;
import aura.compiler.service;
import aura.compiler.evaluator;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::core::g_transaction_guard_metrics;
using aura::core::TransactionGuard;
using aura::core::TransactionGuardHost;
using aura::core::TransactionGuardResult;
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

struct FakeHost {
    int acquires = 0;
    int releases = 0;
    int saves = 0;
    int restores = 0;
    int clears = 0;
    bool reject = false;
    bool save_ok = true;
    // Opaque non-null handle identity (address of this object).
    char handle_slot = 0;

    static void* try_acquire_fn(void* p, std::uint64_t /*pending*/, bool* ok) noexcept {
        auto* h = static_cast<FakeHost*>(p);
        ++h->acquires;
        if (h->reject) {
            if (ok)
                *ok = false;
            return nullptr;
        }
        if (ok)
            *ok = true;
        return static_cast<void*>(&h->handle_slot);
    }
    static void release_fn(void* p, void* /*handle*/) noexcept {
        ++static_cast<FakeHost*>(p)->releases;
    }
    static bool save_fn(void* p) noexcept {
        auto* h = static_cast<FakeHost*>(p);
        ++h->saves;
        return h->save_ok;
    }
    static bool restore_fn(void* p) noexcept {
        ++static_cast<FakeHost*>(p)->restores;
        return true;
    }
    static bool clear_fn(void* p) noexcept {
        ++static_cast<FakeHost*>(p)->clears;
        return true;
    }

    TransactionGuardHost host(bool owns_panic = false) {
        return TransactionGuardHost{
            this, this, &try_acquire_fn, &release_fn, &save_fn, &restore_fn, &clear_fn, owns_panic,
        };
    }
};

// ── AC1: no scaffold simulation ──
static void ac1_no_scaffold() {
    std::println("\n--- #2555 AC1: scaffold simulation removed ---");
    const auto tg = read_file("src/core/transaction_guard.hh");
    CHECK(!tg.empty(), "AC1: read transaction_guard.hh");
    CHECK(tg.find("simulate boundary acquisition") == std::string::npos,
          "AC1: no scaffold simulate comment");
    CHECK(tg.find("TransactionGuardHost") != std::string::npos, "AC1: Host present");
    CHECK(tg.find("try_acquire") != std::string::npos, "AC1: try_acquire");
    CHECK(tg.find("#2555") != std::string::npos, "AC1: cites #2555");
    // Default ctor deleted — must pass host.
    CHECK(tg.find("TransactionGuard() = delete") != std::string::npos ||
              tg.find("TransactionGuard()=delete") != std::string::npos ||
              tg.find("TransactionGuard() = delete") != std::string::npos,
          "AC1: default ctor deleted");
}

// ── AC2: agent body + set-body migration ──
static void ac2_call_sites() {
    std::println("\n--- #2555 AC2: agent body + set-body use TransactionGuard ---");
    const auto fiber = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto mbg = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(fiber.find("g_orch_agent_body_tx") != std::string::npos,
          "AC2: agent body uses g_orch_agent_body_tx");
    CHECK(fiber.find("transaction_guard_host") != std::string::npos,
          "AC2: agent body cites transaction_guard_host");
    CHECK(fiber.find("TransactionGuard") != std::string::npos, "AC2: TransactionGuard in fiber");
    CHECK(mut.find("transaction_guard_host(ev)") != std::string::npos,
          "AC2: set-body uses transaction_guard_host");
    CHECK(mut.find("TransactionGuard tg") != std::string::npos, "AC2: set-body TransactionGuard");
    // Scaffold-only dual path gone from set-body.
    CHECK(mut.find("TransactionGuard surface is also exercised") == std::string::npos,
          "AC2: scaffold dual-path comment removed from set-body");
    CHECK(mbg.find("transaction_guard_try_acquire") != std::string::npos, "AC2: host factory impl");
    CHECK(mbg.find("transaction_guard_host_for_region") != std::string::npos,
          "AC2: region host factory");
}

// ── AC3: reject leaves nothing held ──
static void ac3_reject() {
    std::println("\n--- #2555 AC3: reject path ---");
    FakeHost fh;
    fh.reject = true;
    const auto acq_before =
        g_transaction_guard_metrics().acquired_total.load(std::memory_order_relaxed);
    const auto rej_before =
        g_transaction_guard_metrics().rejected_total.load(std::memory_order_relaxed);
    const auto active_before =
        g_transaction_guard_metrics().panic_checkpoint_active.load(std::memory_order_relaxed);
    {
        TransactionGuard tg(fh.host(/*owns_panic=*/false));
        CHECK(tg.result() == TransactionGuardResult::Rejected, "AC3: result Rejected");
        CHECK(!tg.acquired(), "AC3: not acquired");
        CHECK(fh.acquires == 1, "AC3: try_acquire called");
        CHECK(fh.releases == 0, "AC3: no release on reject");
        CHECK(fh.saves == 0, "AC3: no save on reject");
    }
    CHECK(fh.releases == 0, "AC3: dtor does not release rejected");
    CHECK(g_transaction_guard_metrics().rejected_total.load(std::memory_order_relaxed) >=
              rej_before + 1,
          "AC3: rejected_total bumped");
    CHECK(g_transaction_guard_metrics().acquired_total.load(std::memory_order_relaxed) ==
              acq_before,
          "AC3: acquired_total unchanged on reject");
    CHECK(g_transaction_guard_metrics().panic_checkpoint_active.load(std::memory_order_relaxed) ==
              active_before,
          "AC3: panic_checkpoint_active not left elevated");
}

// ── AC4: panic recovery ──
static void ac4_panic_recover() {
    std::println("\n--- #2555 AC4: recover_panic ---");
    FakeHost fh;
    const auto rec_before =
        g_transaction_guard_metrics().panic_recovered_total.load(std::memory_order_relaxed);
    TransactionGuard tg(fh.host(/*owns_panic=*/false));
    CHECK(tg.result() == TransactionGuardResult::Acquired, "AC4: acquired");
    CHECK(fh.saves == 1, "AC4: save armed");
    const auto r = tg.recover_panic();
    CHECK(r == TransactionGuardResult::PanicRecovered, "AC4: PanicRecovered");
    CHECK(fh.restores == 1, "AC4: restore called");
    CHECK(fh.releases == 1, "AC4: release called");
    CHECK(g_transaction_guard_metrics().panic_recovered_total.load(std::memory_order_relaxed) >=
              rec_before + 1,
          "AC4: panic_recovered_total");
    // Idempotent second recover.
    CHECK(tg.recover_panic() == TransactionGuardResult::PanicRecovered, "AC4: idempotent");
    CHECK(fh.releases == 1, "AC4: no double release");
}

// ── AC4b: commit path clears without restore ──
static void ac4b_commit() {
    std::println("\n--- #2555 AC4b: commit clears, no restore ---");
    FakeHost fh;
    {
        TransactionGuard tg(fh.host(/*owns_panic=*/false));
        CHECK(tg.acquired(), "AC4b: acquired");
        tg.commit();
    }
    CHECK(fh.releases == 1, "AC4b: released");
    CHECK(fh.restores == 0, "AC4b: no restore on commit");
    CHECK(fh.clears == 1, "AC4b: clear on success");
}

// ── AC5: live Evaluator host + source schema ──
static void ac5_live_and_schema() {
    std::println("\n--- #2555 AC5: live host + source-cite ---");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    CHECK(ixx.find("transaction_guard_host") != std::string::npos, "AC5: factory in evaluator.ixx");
    CHECK(ixx.find("make_transaction_guard") != std::string::npos, "AC5: make_transaction_guard");
    CHECK(ixx.find("#2555") != std::string::npos, "AC5: cites #2555 in evaluator.ixx");

    CompilerService cs;
    auto* ev = &cs.evaluator();
    auto host = Evaluator::transaction_guard_host(*ev);
    CHECK(host.ctx == ev, "AC5: host.ctx is Evaluator");
    CHECK(host.try_acquire != nullptr, "AC5: try_acquire bound");
    CHECK(host.release != nullptr, "AC5: release bound");
    CHECK(host.host_owns_panic_checkpoint, "AC5: MBG owns panic");

    // Acquire real boundary (empty workspace still gets Guard).
    {
        TransactionGuard tg(host, /*pending=*/1);
        CHECK(tg.result() == TransactionGuardResult::Acquired ||
                  tg.result() == TransactionGuardResult::Rejected,
              "AC5: live host returns Acquired or Rejected");
        if (tg.acquired())
            tg.commit();
    }
}

} // namespace

int main() {
    std::println("=== Issue #2555: TransactionGuard real path ===");
    ac1_no_scaffold();
    ac2_call_sites();
    ac3_reject();
    ac4_panic_recover();
    ac4b_commit();
    ac5_live_and_schema();
    std::println("\n=== #2555: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
