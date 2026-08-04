// @category: unit
// @reason: Issue #2530 — Capability + Isolation kAuditRing=1024 + Isolation publish_seq.
//
//   AC1: both kAuditRing == 1024
//   AC2: Isolation write exclusive + publish_seq; try_load double-check
//   AC3: >1024 denials retain latest window (source-cite WAL for earlier)
//   AC4: Soft path zero extra (source-cite)
//   AC5: concurrent record + try_load (smoke)
//   AC6: source-cite + linter

#include "test_harness.hpp"
#include "core/capability_model.hh"
#include "core/workspace_isolation.hh"
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;

namespace {
using aura::core::capability::CapabilityRegistry;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::IsolationAuditEntry;
using aura::core::workspace_isolation::WorkspaceIsolationPolicy;
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
} // namespace

int run_test_audit_ring_publish() {
    std::println("=== Issue #2530: audit ring 1024 + Isolation publish ===");
    CHECK(CapabilityRegistry::kAuditRing == 1024, "AC1: capability ring 1024");
    CHECK(WorkspaceIsolationPolicy::kAuditRing == 1024, "AC1: isolation ring 1024");

    {
        std::println("\n--- AC2: Isolation publish + try_load ---");
        g_workspace_isolation().clear_for_test();
        g_workspace_isolation().set_current_tenant(1, "t1");
        g_workspace_isolation().record_audit(2, 0, true, false, true, "test-deny", 0);
        IsolationAuditEntry e{};
        const auto seq = g_workspace_isolation().load_audit_seq();
        CHECK(seq >= 1, "seq advanced");
        CHECK(g_workspace_isolation().try_load_audit_seq(seq - 1, e), "try_load ok");
        CHECK(e.denied, "denied");
        CHECK(e.mutation_id != 0, "mid stamped");
    }
    {
        std::println("\n--- AC3/AC5: many records + concurrent try_load ---");
        g_workspace_isolation().clear_for_test();
        g_workspace_isolation().set_current_tenant(1, "t1");
        for (int i = 0; i < 1100; ++i)
            g_workspace_isolation().record_audit(2, 0, true, false, false, "storm", 0);
        const auto seq = g_workspace_isolation().load_audit_seq();
        IsolationAuditEntry e{};
        CHECK(g_workspace_isolation().try_load_audit_seq(seq - 1, e), "latest loadable");
        std::atomic<int> ok{0};
        std::vector<std::thread> ths;
        for (int t = 0; t < 4; ++t) {
            ths.emplace_back([&] {
                IsolationAuditEntry x{};
                for (int i = 0; i < 200; ++i) {
                    if (g_workspace_isolation().try_load_audit_seq(seq - 1 - (i % 10), x))
                        ok.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& t : ths)
            t.join();
        CHECK(ok.load() > 0, "AC5: concurrent try_load progressed");
    }
    {
        std::println("\n--- AC6: source-cite ---");
        auto cap = read_file("src/core/capability_model.hh");
        auto iso = read_file("src/core/workspace_isolation.hh");
        CHECK(cap.find("kAuditRing = 1024") != std::string::npos, "cap 1024");
        CHECK(iso.find("kAuditRing = 1024") != std::string::npos, "iso 1024");
        CHECK(iso.find("PublishedIsolationSlot") != std::string::npos, "PublishedIsolationSlot");
        CHECK(iso.find("try_load_audit_seq") != std::string::npos, "try_load");
        CHECK(iso.find("2530") != std::string::npos, "cite #2530");
        CHECK(cap.find("2530") != std::string::npos, "cap cite #2530");
    }
    std::println("\n=== #2530: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_audit_ring_publish();
}
#endif
