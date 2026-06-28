// test_hardware_roadmap_acknowledged.cpp — Issue #304:
// Aura → Agentic EDA Roadmap (acknowledgment + docs).
//
// Issue #304 is a roadmap/vision issue, not a shipping task.
// This binary acknowledges it by verifying the canonical
// roadmap doc is present + has the required sections + links
// back to the source issue. This treats the roadmap as the
// "spec" we accepted, with future child issues filed per
// phase (Phase 0/1/2) — see docs/ROADMAP-HARDWARE.md.

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

import std;

namespace aura_issue_304_detail {

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::string content;
    if (f.good()) {
        content.assign((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
    }
    f.close();
    return content;
}

// ── AC1: docs/ROADMAP-HARDWARE.md present
bool test_roadmap_doc_present() {
    std::println("\n--- AC1: docs/ROADMAP-HARDWARE.md present ---");
    std::string content =
        read_file("/home/dev/code/aura/docs/ROADMAP-HARDWARE.md");
    CHECK(!content.empty(), "docs/ROADMAP-HARDWARE.md exists on disk");
    std::println("  doc length: {} chars", content.size());
    CHECK(content.size() > 1000,
          "roadmap doc has substantive content (>= 1000 chars)");
    return true;
}

// ── AC2: doc has all 3 phase sections (Phase 0/1/2)
bool test_roadmap_phases_present() {
    std::println("\n--- AC2: roadmap doc has all 3 phases ---");
    std::string content =
        read_file("/home/dev/code/aura/docs/ROADMAP-HARDWARE.md");
    const bool has_phase0 = content.find("Phase 0") != std::string::npos;
    const bool has_phase1 = content.find("Phase 1") != std::string::npos;
    const bool has_phase2 = content.find("Phase 2") != std::string::npos;
    std::println("  phases: Phase 0={} Phase 1={} Phase 2={}",
                 has_phase0, has_phase1, has_phase2);
    CHECK(has_phase0, "doc has Phase 0 section");
    CHECK(has_phase1, "doc has Phase 1 section");
    CHECK(has_phase2, "doc has Phase 2 section");
    return true;
}

// ── AC3: doc references Issue #304 as source + decision
//         docs + risks/success metrics sections
bool test_roadmap_required_sections() {
    std::println("\n--- AC3: roadmap doc has required sections + Issue #304 ref ---");
    std::string content =
        read_file("/home/dev/code/aura/docs/ROADMAP-HARDWARE.md");
    const bool has_304_ref =
        content.find("Issue #304") != std::string::npos;
    const bool has_risks = content.find("Risks") != std::string::npos ||
        content.find("Risk") != std::string::npos;
    const bool has_success =
        content.find("Success metric") != std::string::npos;
    const bool has_cross_refs =
        content.find("Cross-references") != std::string::npos;
    const bool has_decision_doc = content.find(
        "primitive-vs-stdlib-decision-framework.md") != std::string::npos;
    std::println("  required sections: 304_ref={} risks={} success={} "
                 "cross_refs={} decision_doc={}",
                 has_304_ref, has_risks, has_success,
                 has_cross_refs, has_decision_doc);
    CHECK(has_304_ref, "doc references Issue #304 as source");
    CHECK(has_risks, "doc has Risks section");
    CHECK(has_success, "doc has Success metric section");
    CHECK(has_cross_refs, "doc has Cross-references section");
    CHECK(has_decision_doc,
          "doc cross-references the decision framework");
    return true;
}

// ── AC4: Status table present (3 phases + child-issues
//         pending per phase) — the actionable follow-up
bool test_roadmap_status_table() {
    std::println("\n--- AC4: roadmap Status table present ---");
    std::string content =
        read_file("/home/dev/code/aura/docs/ROADMAP-HARDWARE.md");
    const bool has_status = content.find("| Phase |") != std::string::npos;
    const bool has_tbd = content.find("TBD") != std::string::npos;
    const bool has_pending = content.find("child issue pending")
        != std::string::npos;
    std::println("  Status table: status={} tbd={} pending={}",
                 has_status, has_tbd, has_pending);
    CHECK(has_status, "doc has Status table (| Phase | | Status | | ... |)");
    CHECK(has_tbd, "doc has TBD markers (phases pending)");
    CHECK(has_pending, "doc has 'child issue pending' markers");
    return true;
}

int run_tests() {
    std::println("═══ Issue #304 verification tests ═══\n");
    std::println("Layer 1: roadmap doc presence");
    test_roadmap_doc_present();
    std::println("\nLayer 2: phase sections");
    test_roadmap_phases_present();
    std::println("\nLayer 3: required sections + Issue #304 ref");
    test_roadmap_required_sections();
    std::println("\nLayer 4: Status table");
    test_roadmap_status_table();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_304_detail

int aura_issue_304_run() { return aura_issue_304_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_304_run(); }
#endif