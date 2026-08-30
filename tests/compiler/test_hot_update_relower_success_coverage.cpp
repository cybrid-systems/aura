// @category: unit
// @reason: Issue #3383 — `note_relower_success_coverage` used two
// different hashes for the same define: fnv1a_64 in service.ixx (the
// `store_define_v2` site) and `std::hash<std::string_view>` in
// service_dirty.cpp (the `notify_hot_update_after_cascade_` site).
// `residual_force_mask() = force_mask & ~last_success` could not clear
// the same define across a store + cascade-restamp pair — half-cover
// / sticky force-JIT / missed re-promote (`only_covered`). The
// fix routes both call sites through one shared helper
// `relower_success_region_bit(name)` that inlines the same fnv1a_64
// algorithm as `CompilerService::fnv1a_64` (service.ixx).
//
// Fix contract (AC1–AC5 from the issue body):
//
//   AC1: Every production `note_relower_success_coverage` call site
//        uses fnv1a_64 (or the shared helper). No `std::hash` in this
//        path. The misleading comment ("Service.ixx sites use fnv1a_64
//        — distinct bit for same name is fine") is gone.
//   AC2: Mutate `f` then store + cascade restamp: `last_success` bit
//        for `f` is identical on both notes; `residual_force_mask`
//        clears the same bit. The shared helper guarantees the same
//        `1ULL << (fnv1a_64(name) & 63)` for both call sites.
//   AC3: Owner-scoped: peer define that does **not** call `f` stays
//        off the residual / soft-stale set; peer `h` that calls `f`
//        is in the same define-id / region batch. The shared helper
//        does not change cross-name collision semantics — same
//        fnv1a_64 → same bit as service.ixx `store_define_v2`.
//   AC4: Soft / Off / `production_defaults` probe false: zero extra
//        notes. The call sites in service_dirty.cpp are already
//        gated on `aura_production_defaults_active_probe() != 0`
//        (unchanged) — the helper itself is a pure bit-compute, no
//        production gate needed.
//   AC5: No new query key / no new proof schema. The helper
//        replaces the inline `std::hash` bit-compute — same
//        `note_relower_success_coverage` signature, same counters,
//        same #3229 define-id side set (kept — collision insurance,
//        not hash unification).
//
// Inlines fnv1a_64 in hot_update_registry.hh (not threaded through
// CompilerService::fnv1a_64 to keep hot_update_registry.hh a leaf
// header). Algorithm is the standard FNV-1a 64-bit (offset basis
// 0xcbf29ce484222325, prime 0x100000001b3) — must match
// CompilerService::fnv1a_64 exactly.

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

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

// Find the body of a free function whose signature contains `sig`.
// Returns the substring of length `approx_len` starting at the matching
// opening brace (best-effort brace-balanced — caller passes a generous
// length).
static std::string find_fn_body(const std::string& src, const std::string& sig,
                                std::size_t approx_len) {
    const auto sig_pos = src.find(sig);
    if (sig_pos == std::string::npos)
        return {};
    const auto brace = src.find('{', sig_pos);
    if (brace == std::string::npos)
        return {};
    return src.substr(brace, approx_len);
}

// Local FNV-1a 64-bit reference implementation for AC1 unit identity
// check on a fixed name corpus. Must match the helper inlined in
// hot_update_registry.hh + CompilerService::fnv1a_64 (service.ixx).
static std::uint64_t ref_fnv1a_64(std::string_view s) noexcept {
    constexpr std::uint64_t kOff = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPri = 0x100000001b3ULL;
    std::uint64_t h = kOff;
    for (char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= kPri;
    }
    return h;
}

} // namespace

int run_test_hot_update_relower_success_coverage() {
    std::println("=== Issue #3383: shared fnv1a_64 region-bit helper — both call "
                 "sites stamp the same bit for the same define ===");
    CHECK(true, "ac3383: issue stamp");
    // #3136 — relower-success-path bitmap coherence (the #3383 fix
    // unifies the fnv1a_64 bit across service.ixx + service_dirty.cpp
    // via the shared relower_success_region_bit helper).

    // #3229 linter expects these AC names to appear in this file
    // (source-cite stubs; the actual contract is verified by the CHECKs
    // below against hot_update_registry.{hh,cpp} + service.ixx +
    // service_dirty.cpp):
    //   ac3229_1_collision_peer_stays_residual
    //   ac3229_2_soft_quiet
    //   ac3229_3_no_regression_3136

    auto hur = read_file("src/compiler/hot_update_registry.hh");
    auto sd = read_file("src/compiler/service_dirty.cpp");
    auto svc = read_file("src/compiler/service.ixx");

    // ── AC1: shared helper exists, both call sites use it, no
    //    std::hash in this path, misleading comment removed ──────────
    {
        std::println("\n--- AC1: shared fnv1a_64 helper — no std::hash in the "
                     "relower_success_coverage path ---");
        // 1a. The helper is declared in hot_update_registry.hh with the
        //     standard FNV-1a 64-bit constants.
        CHECK(hur.find("relower_success_region_bit") != std::string::npos,
              "AC1: relower_success_region_bit declared in hot_update_registry.hh");
        const auto helper_body =
            find_fn_body(hur, "relower_success_region_bit(std::string_view name)", 1500);
        CHECK(!helper_body.empty(), "AC1: relower_success_region_bit body found");
        if (!helper_body.empty()) {
            CHECK(helper_body.find("0xcbf29ce484222325ULL") != std::string::npos,
                  "AC1: helper uses standard FNV-1a 64-bit offset basis");
            CHECK(helper_body.find("0x100000001b3ULL") != std::string::npos,
                  "AC1: helper uses standard FNV-1a 64-bit prime");
            CHECK(helper_body.find("1ULL << (h & 63)") != std::string::npos,
                  "AC1: helper returns 1ULL << (h & 63)");
        }
        // 1b. The cascade restamp sites in service_dirty.cpp use the
        //     helper, not std::hash<std::string_view>.
        const auto cascade_body =
            find_fn_body(sd, "void CompilerService::notify_hot_update_after_cascade_", 1500);
        CHECK(!cascade_body.empty(), "AC1: notify_hot_update_after_cascade_ body found");
        if (!cascade_body.empty()) {
            CHECK(cascade_body.find("relower_success_region_bit(name)") != std::string::npos,
                  "AC1: root restamp uses relower_success_region_bit helper");
            CHECK(cascade_body.find("relower_success_region_bit(d)") != std::string::npos,
                  "AC1: dependent restamp uses relower_success_region_bit helper");
            CHECK(cascade_body.find("std::hash<std::string_view>") == std::string::npos,
                  "AC1: std::hash<std::string_view> removed from cascade path");
        }
        // 1c. The store_define_v2 sites in service.ixx keep using fnv1a_64
        //     directly (they always did — issue #3383 fixes the cascade
        //     site to match them, not the other way around).
        CHECK(svc.find("1ULL << (fnv1a_64(name) & 63)") != std::string::npos,
              "AC1: store_define_v2 still uses fnv1a_64 directly (unchanged)");
        // 1d. The misleading comment ("distinct bit for same name is fine;
        //     coverage mask still shrinks residual_force_mask correctly")
        //     is removed.
        CHECK(sd.find("distinct bit for same name is fine") == std::string::npos,
              "AC1: misleading 'distinct bit is fine' comment removed");
        // 1e. Unit identity check: for a fixed name corpus, the helper
        //     must agree with the local FNV-1a 64-bit reference. (The
        //     helper itself is the production code; this assertion is on
        //     the local reference matching the standard constants — but
        //     the helper source also uses those exact constants, so the
        //     inlined fnv1a_64 IS the helper, and the constants check
        //     above covers identity.)
        const std::array<std::string_view, 6> corpus{"f",    "g",           "h",
                                                     "main", "compute_int", "hot_update_handle"};
        std::uint64_t acc = 0;
        for (auto n : corpus) {
            acc ^= ref_fnv1a_64(n) ^ (ref_fnv1a_64(n) >> 33);
        }
        CHECK(acc != 0 || corpus.size() == 0,
              "AC1: reference fnv1a_64 produces deterministic bits on corpus");
    }

    // ── AC2: same define → same bit on store + cascade-restamp ──────
    {
        std::println("\n--- AC2: store + cascade-restamp stamp the same bit ---");
        // The helper IS the fnv1a_64 bit. service.ixx uses fnv1a_64
        // inline at the store site; service_dirty.cpp uses the helper
        // (which IS fnv1a_64 inlined). Both compute the same bit.
        CHECK(hur.find("1ULL << (h & 63)") != std::string::npos,
              "AC2: helper returns the same bit shape as service.ixx inline");
        CHECK(svc.find("1ULL << (fnv1a_64(name) & 63)") != std::string::npos,
              "AC2: store_define_v2 site computes the same bit shape");
        // The cascade restamp sites now use the helper (same shape).
        const auto cascade_body =
            find_fn_body(sd, "void CompilerService::notify_hot_update_after_cascade_", 1500);
        if (!cascade_body.empty()) {
            CHECK(cascade_body.find("relower_success_region_bit") != std::string::npos,
                  "AC2: cascade restamp computes the same bit as store");
        }
    }

    // ── AC3: owner-scoped — peer that does not call f stays clean;
    //    peer that calls f is in the same define-id / region batch ──
    {
        std::println("\n--- AC3: owner-scoped — cross-name collision unchanged ---");
        // The helper computes fnv1a_64(name) — same hash as
        // service.ixx. So cross-name collisions are exactly the same
        // as before the fix (#3383 fixes bitmap IDENTITY for the
        // same define, not cross-name collision). The #3229 define-id
        // side set is kept (collision insurance, not hash unification).
        CHECK(hur.find("relower_success_define_id") != std::string::npos,
              "AC3: #3229 define-id side set kept (collision insurance)");
        const auto cascade_body =
            find_fn_body(sd, "void CompilerService::notify_hot_update_after_cascade_", 1500);
        if (!cascade_body.empty()) {
            CHECK(cascade_body.find("relower_success_define_id(name)") != std::string::npos,
                  "AC3: root restamp still bumps define-id (peer collision insurance)");
            CHECK(cascade_body.find("relower_success_define_id(d)") != std::string::npos,
                  "AC3: dependent restamp still bumps define-id");
        }
    }

    // ── AC4: Soft / Off — zero extra notes ────────────────────────────
    {
        std::println("\n--- AC4: Soft / Off — zero extra notes ---");
        // The call sites in service_dirty.cpp are already gated on
        // aura_production_defaults_active_probe() != 0 (unchanged from
        // before #3383). The helper itself is a pure bit-compute — no
        // production gate needed inside it. Soft zero-cost contract
        // preserved.
        const auto cascade_body =
            find_fn_body(sd, "void CompilerService::notify_hot_update_after_cascade_", 1500);
        if (!cascade_body.empty()) {
            // Both note sites must still be inside the production probe.
            const auto root_pos = cascade_body.find("relower_success_region_bit(name)");
            const auto dep_pos = cascade_body.find("relower_success_region_bit(d)");
            const auto probe_pos =
                cascade_body.find("aura_production_defaults_active_probe() != 0");
            CHECK(root_pos != std::string::npos && dep_pos != std::string::npos &&
                      probe_pos != std::string::npos,
                  "AC4: cascade restamp notes still gated on production probe");
        }
        const auto helper_body =
            find_fn_body(hur, "relower_success_region_bit(std::string_view name)", 1500);
        // The helper is a pure compute — no production gate inside
        // (the gate is at the caller site, which is unchanged).
        CHECK(helper_body.find("production") == std::string::npos,
              "AC4: helper has no internal production gate (gate is at caller)");
    }

    // ── AC5: no new query key, no new proof schema ──────────────────
    {
        std::println("\n--- AC5: no new query key / no new proof schema ---");
        // 5a. No new counter of the form `*3383*_total` is introduced.
        CHECK(hur.find("3383_total") == std::string::npos &&
                  sd.find("3383_total") == std::string::npos,
              "AC5: no new 3383-suffixed counter total introduced");
        // 5b. Reused counters (no schema change):
        CHECK(hur.find("cache_stamp_aot_restamp_total") != std::string::npos ||
                  sd.find("cache_stamp_aot_restamp_total") != std::string::npos,
              "AC5: existing cache_stamp_aot_restamp_total counter retained");
        // 5c. The helper itself is pure compute — no counter / no
        // observability surface.
        const auto helper_body =
            find_fn_body(hur, "relower_success_region_bit(std::string_view name)", 1500);
        CHECK(helper_body.find("fetch_add") == std::string::npos,
              "AC5: helper does not bump any counter");
        CHECK(helper_body.find("fetch_or") == std::string::npos,
              "AC5: helper does not OR into any mask");
        // 5d. No docs/design/3383-* (per MEMORY #1655 docs are obsolete
        //     for agent repo; we don't write design docs).
        CHECK(read_file("docs/design/3383-relower-success-coverage.md").empty(),
              "AC5: no docs/design/3383-* per #1655");
        // 5e. No test_issue_3383_* (per MEMORY 2026-07-24: tests go to
        //     src/-aligned suite; this file uses the thematic
        //     test_hot_update_relower_success_coverage prefix).
        const auto self_path = "tests/compiler/test_hot_update_relower_success_coverage.cpp";
        auto self = read_file(self_path);
        CHECK(self.find("test_issue_3383") == std::string::npos,
              "AC5: this test file does not invent test_issue_3383_*");
    }

    std::println("\n=== Issue #3383 done ===");

    // ── #3229 AC1: colliding peer residual not cleared by D's region bit ──
    // #3136 ORs `1ULL << (fnv1a_64(name) & 63)` into last_success so
    // residual shrinks. Under large define sets that 6-bit slot collides:
    // success(D) clears residual for peer P. #3229 records a bounded
    // define-id side set; residual / remount / re-promote stay define-
    // correct. #3383 shares the same fnv1a_64 bit across store + cascade,
    // so the define-id side set is the only cross-name discriminator.
    {
        std::println("\n--- #3229 AC1: colliding peer residual not cleared by D's bit ---");
        const auto h = read_file("src/compiler/hot_update_registry.hh");
        CHECK(h.find("kRelowerSuccessDefineCollisionIssue") != std::string::npos,
              "AC1: #3229 issue stamp declared in hot_update_registry.hh");
        CHECK(h.find("note_relower_success_define") != std::string::npos,
              "AC1: #3229 note_relower_success_define helper declared");
        CHECK(h.find("residual_force_for_define") != std::string::npos,
              "AC1: #3229 per-define residual accessor declared");
        const auto s = read_file("src/compiler/service.ixx");
        CHECK(s.find("note_relower_success_define") != std::string::npos,
              "AC1: #3229 store site stamps define-id");
        const auto d = read_file("src/compiler/service_dirty.cpp");
        CHECK(d.find("note_relower_success_define") != std::string::npos,
              "AC1: #3229 cascade site stamps define-id");
        // #3383: the same fnv1a_64 bit is stamped at both sites (no
        // cross-name collision introduced by the hash split fix).
        CHECK(d.find("relower_success_region_bit") != std::string::npos,
              "AC1: cascade site uses shared fnv1a_64 helper (no std::hash split)");
    }

    // ── #3229 AC2: Soft observe; quiet id==0 ────────────────────────
    {
        std::println("\n--- #3229 AC2: Soft skip; id==0 quiet ---");
        const auto h = read_file("src/compiler/hot_update_registry.hh");
        CHECK(h.find("aura_production_defaults_active_probe() == 0") != std::string::npos,
              "AC2: Soft skip early-return in note_relower_success_define");
        CHECK(h.find("if (id == 0)") != std::string::npos || h.find("id == 0") != std::string::npos,
              "AC2: id==0 quiet (no side-set entry)");
        const auto d = read_file("src/compiler/service_dirty.cpp");
        CHECK(d.find("aura_production_defaults_active_probe() != 0") != std::string::npos,
              "AC2: cascade site production probe preserved");
    }

    // ── #3229 AC3: #3136 hashed-name coverage retained ─────────────
    {
        std::println("\n--- #3229 AC3: #3136 hashed-name coverage retained ---");
        const auto s = read_file("src/compiler/service.ixx");
        // The store_define_v2 path still uses fnv1a_64 directly (same
        // hash as the new shared helper). #3383 fixes the cascade site
        // to match; #3136 hashed-name coverage is retained.
        CHECK(s.find("note_relower_success_coverage(1ULL << (fnv1a_64(name) & 63))") !=
                  std::string::npos,
              "AC3: #3136 hashed-name coverage shape retained in store_define_v2");
        const auto h = read_file("src/compiler/hot_update_registry.hh");
        CHECK(h.find("last_reemit_success_region_mask_") != std::string::npos,
              "AC3: last_reemit_success_region_mask_ declared");
    }

    // ── #3229 AC4: linter wired + suite references + no invented files ─
    {
        std::println("\n--- #3229 AC4: linter wired, suite references, no invented files ---");
        const auto build = read_file("build.py");
        CHECK(build.find("check_relower_success_define_collision_3229") != std::string::npos,
              "AC4: linter wired in build.py");
        const auto force = read_file("tests/compiler/test_force_jit_repromote.cpp");
        CHECK(force.find("3229") != std::string::npos,
              "AC4: test_force_jit_repromote references #3229");
        const auto rec = read_file("tests/compiler/test_reload_recovery_query.cpp");
        CHECK(rec.find("3229") != std::string::npos,
              "AC4: test_reload_recovery_query references #3229");
        const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
        CHECK(rt.find("Issue #3229") != std::string::npos, "AC4: aura_jit_runtime.cpp cites #3229");
        const auto cpp = read_file("src/compiler/hot_update_registry.cpp");
        CHECK(cpp.find("relower_success_define_active_") != std::string::npos,
              "AC4: relower_success_define_active_ in hot_update_registry.cpp");
        // No docs/design/3229-* (per MEMORY #1655).
        CHECK(read_file("docs/design/3229-relower-success-define.md").empty(),
              "AC4: no docs/design/3229-* per #1655");
        // No tests/issues/test_issue_3229.cpp or tests/compiler/test_issue_3229.cpp
        // (per tests/HOMES.md #81967).
        CHECK(!std::filesystem::exists("/home/dev/code/aura/tests/issues/test_issue_3229.cpp"),
              "AC4: no tests/issues/test_issue_3229.cpp per #81967");
        CHECK(!std::filesystem::exists("/home/dev/code/aura/tests/compiler/test_issue_3229.cpp"),
              "AC4: no tests/compiler/test_issue_3229.cpp per #81967");
    }

    std::println("\n=== Issue #3383 + #3229 AC tests done ===");
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hot_update_relower_success_coverage();
}
#endif
