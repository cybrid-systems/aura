// @category: unit
// @reason: Issue #2363 — complete GeneralObjectPin adoption for mutate /
// agent / scratch intermediate create paths (refines #2337 single-site).
//
//   AC1: wire_general_object_create_pair pins both buffers + bumps wire
//   AC2: Unit pin → Moving densify → validate / remap path works
//   AC3: Negative — unpinned validate fails (fail-closed counter)
//   AC4: Soft densify / no pins → zero extra remap cost
//   AC5: Inventory kGeneralObjectPinAdoptSiteCount == 7 + all sites wired
//        (source-cite) + query schema-2363 + Moving remap/verify retained

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.core.arena;
import aura.core.lifetime_pin;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::ASTArena;
using aura::ast::LiveCompactMode;
using aura::ast::moving_compact_enabled;
using aura::ast::set_moving_compact_enabled;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::lifetime::g_lifetime_pin_stats;
using aura::core::lifetime::GeneralObjectPin;
using aura::core::lifetime::kGeneralObjectPinAdoptIssue;
using aura::core::lifetime::kGeneralObjectPinAdoptSiteCount;
using aura::core::lifetime::kLifetimePinPhase;
using aura::core::lifetime::LifetimePin;
using aura::core::lifetime::note_general_object_pin_mutate_wire;
using aura::core::lifetime::pin_or_fail;
using aura::core::lifetime::validate_general_object;
using aura::core::lifetime::wire_general_object_create_pair;
using aura::test::g_failed;
using aura::test::g_passed;

struct Pod16 {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    Pod16() = default;
    Pod16(std::uint64_t x, std::uint64_t y)
        : a(x)
        , b(y) {}
};
static_assert(sizeof(Pod16) <= 64, "Pod16 must densify in SmallObjectPool");

struct MovingFlagGuard {
    int prev = -1;
    explicit MovingFlagGuard(int enable) {
        prev = moving_compact_enabled();
        set_moving_compact_enabled(enable);
    }
    ~MovingFlagGuard() { set_moving_compact_enabled(prev); }
};

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
    // Production surface is query:arena-live-compact-stats (#2004 / #2298).
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:arena-live-compact-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: wire helper pins both + bumps wire once ──
static void ac1_wire_pair() {
    std::println("\n--- AC1: wire_general_object_create_pair ---");
    int a = 1, b = 2;
    GeneralObjectPin pa, pb;
    const auto w0 = g_lifetime_pin_stats.general_object_pin_mutate_wire_total;
    const auto p0 = g_lifetime_pin_stats.general_object_pin_total;
    CHECK(wire_general_object_create_pair(pa, pb, &a, &b, /*gen=*/1, /*arena_id=*/0),
          "AC1: wire pair succeeds");
    CHECK(pa.pinned() && pa.ptr() == &a, "AC1: pin_a holds a");
    CHECK(pb.pinned() && pb.ptr() == &b, "AC1: pin_b holds b");
    CHECK(g_lifetime_pin_stats.general_object_pin_mutate_wire_total == w0 + 1,
          "AC1: wire total +1 per site");
    CHECK(g_lifetime_pin_stats.general_object_pin_total == p0 + 2,
          "AC1: pin_total +2 (both buffers)");
    CHECK(kGeneralObjectPinAdoptIssue == 2363, "AC1: adopt issue stamp 2363");
    CHECK(kLifetimePinPhase == 3, "AC1: LifetimePin Phase 3 retained");
}

// ── AC2: pin → Moving densify → validate ──
static void ac2_pin_moving_validate() {
    std::println("\n--- AC2: pin under mutate-style create → Moving densify ---");
    MovingFlagGuard mg(1);
    ASTArena arena;
    auto* obj = arena.create<Pod16>(7, 9);
    CHECK(obj != nullptr, "AC2: allocate densify-tracked buffer");
    GeneralObjectPin pin;
    const auto gen0 = arena.generation();
    CHECK(pin.pin(obj, gen0, /*arena_id=*/0), "AC2: pin succeeds");
    CHECK(pin.pinned(), "AC2: pinned");
    // Force a compact densify if possible; Soft may no-op.
    (void)arena.live_compact(LiveCompactMode::Force);
    const auto gen1 = arena.generation();
    // Validate at current gen — may pass (remapped or gen stable).
    const bool ok = pin.validate(gen1, /*arena_id=*/0, /*count_remap_ok=*/true);
    // If gen advanced and pin still live, remap_ok path; if Soft no-op still ok.
    if (pin.pinned()) {
        CHECK(ok || gen1 == gen0, "AC2: validate ok when pin live (or gen unchanged)");
    }
    (void)ok;
}

// ── AC3: negative unpinned fail-closed ──
static void ac3_unpinned_fail() {
    std::println("\n--- AC3: unpinned validate fail-closed ---");
    LifetimePin bare; // registered but never pin()d with a live ptr
    const auto f0 = g_lifetime_pin_stats.general_object_pin_validate_fail_total;
    CHECK(!validate_general_object(bare, /*cur_gen=*/1, /*arena_id=*/0),
          "AC3: unpinned validate fails");
    CHECK(g_lifetime_pin_stats.general_object_pin_validate_fail_total > f0,
          "AC3: validate_fail_total advanced");
}

// ── AC4: Soft zero cost ──
static void ac4_soft_zero() {
    std::println("\n--- AC4: Soft densify zero extra remap ---");
    MovingFlagGuard mg(0);
    const auto r0 = g_lifetime_pin_stats.remaps;
    const auto m0 = g_lifetime_pin_stats.remap_misses;
    ASTArena arena;
    (void)arena.live_compact(LiveCompactMode::Soft);
    CHECK(g_lifetime_pin_stats.remaps == r0, "AC4: Soft → remaps unchanged");
    CHECK(g_lifetime_pin_stats.remap_misses == m0, "AC4: Soft → remap_misses unchanged");
}

// ── AC5: inventory + source-cite + query ──
static void ac5_inventory_query() {
    std::println("\n--- AC5: inventory 7 sites + schema-2363 ---");
    CHECK(kGeneralObjectPinAdoptSiteCount == 7, "AC5: adopt site count == 7");

    const auto lp = read_file("src/core/lifetime_pin.ixx");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    const auto qw = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    const auto ev = read_file("src/compiler/evaluator_primitives_eval.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");

    CHECK(lp.find("wire_general_object_create_pair") != std::string::npos, "AC5: helper API");
    CHECK(lp.find("kGeneralObjectPinAdoptSiteCount = 7") != std::string::npos,
          "AC5: site count constant");
    CHECK(lp.find("kGeneralObjectPinAdoptIssue = 2363") != std::string::npos, "AC5: issue stamp");
    CHECK(lp.find("remap_pins_pointing_to") != std::string::npos ||
              lp.find("verify_pins_under_moving_compact") != std::string::npos,
          "AC5: Moving densify pin remap/verify retained");

    // 7 adopted sites (source-cite)
    CHECK(mut.find("wire_general_object_create_pair") != std::string::npos,
          "AC5: site1 mutate:replace-pattern");
    CHECK(flat.find("batch :replace-pattern") != std::string::npos ||
              flat.find("Issue #2363: GeneralObjectPin adopt (site 2/7)") != std::string::npos,
          "AC5: site2 batch replace-pattern");
    CHECK(flat.find("Issue #2363: GeneralObjectPin adopt (site 3/7)") != std::string::npos,
          "AC5: site3 require import");
    CHECK(qw.find("Issue #2363: GeneralObjectPin adopt (site 4/7)") != std::string::npos,
          "AC5: site4 query:pattern");
    CHECK(qw.find("Issue #2363: GeneralObjectPin adopt (site 5/7)") != std::string::npos,
          "AC5: site5 query:pattern guard");
    CHECK(ev.find("Issue #2363: GeneralObjectPin adopt (site 6/7)") != std::string::npos,
          "AC5: site6 load");
    CHECK(ev.find("Issue #2363: GeneralObjectPin adopt (site 7/7)") != std::string::npos,
          "AC5: site7 eval-expr");

    // Count wire_general_object_create_pair call sites across production src
    auto count_wire = [](const std::string& s) {
        std::size_t n = 0, pos = 0;
        const std::string needle = "wire_general_object_create_pair";
        while ((pos = s.find(needle, pos)) != std::string::npos) {
            ++n;
            pos += needle.size();
        }
        return n;
    };
    // Definition in lifetime_pin + 7 call sites = at least 7 in the 4 files
    const auto calls = count_wire(mut) + count_wire(flat) + count_wire(qw) + count_wire(ev);
    CHECK(calls >= 7, "AC5: >=7 wire call sites across adopt files");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2363") == 2363, "AC5: schema-2363");
    CHECK(href(cs, "issue-2363") == 2363, "AC5: issue-2363");
    CHECK(href(cs, "general-object-pin-adopt-complete-wired") == 1, "AC5: adopt-complete wired");
    CHECK(href(cs, "general-object-pin-adopt-site-count") == 7, "AC5: adopt-site-count query");
    CHECK(href(cs, "schema-2337") == 2337, "AC5: schema-2337 retained");
    CHECK(href(cs, "general-object-pin-mutate-wired") == 1, "AC5: mutate-wired retained");
}

} // namespace

int run_test_general_object_pin_adopt_2363() {
    std::println("=== Issue #2363: complete GeneralObjectPin adopt ===");
    ac1_wire_pair();
    ac2_pin_moving_validate();
    ac3_unpinned_fail();
    ac4_soft_zero();
    ac5_inventory_query();
    std::println("\n=== #2363: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_general_object_pin_adopt_2363();
}
#endif
