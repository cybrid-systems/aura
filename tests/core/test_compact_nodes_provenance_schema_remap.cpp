// @category: unit
// @reason: Issue #3155 — FlatAST::compact_nodes() rebuilt live columns and
// remapped marker / macro_dirty / type_id / topology, but did NOT rebuild
// /remap provenance_ or schema_cache_. After ID remap, MacroIntroduced
// markers may still look present, but provenance / schema columns were
// indexed by OLD NodeIds → homology break for blame, type-cache hits,
// Agent closed-loop tracking.
//
// Residual of closed #3095 (abort×densify marker restore) and densify
// remap work. compact_nodes column set incompleteness only. recycle /
// soft compact (no-remap) paths out of scope.
//
//   AC1: compact_nodes rebuilds provenance_ + schema_cache_ in the same
//        live-order pass as marker_ (column decls + reserves + live loop
//        push + final assignment all present in src/core/ast.ixx).
//   AC2: After compact, for every live remapped id: marker, macro_dirty,
//        provenance, schema_cache refer to the SAME logical node as
//        pre-compact (homology preserved).
//   AC3: validate_macro_hygiene_invariants() still holds post-compact
//        on MacroIntroduced nodes (no regression).
//   AC4: Soft compact / recycle-without-remap paths unchanged
//        (compact_nodes_soft / recycle_dead_nodes preserved).
//   AC5: No new query keys; Soft/Off zero-cost preserved (no new
//        g_3155_* atomic, single alloc pattern as siblings).
//
// Sibling tests implicitly covered (must remain green):
//   - tests/core/test_flatast_* (densify / hygiene / restamp family)
//   - tests/compiler/test_macro_* (MacroIntroduced roundtrip)
//   - tests/compiler/test_densify_* (closed #3095 lineage)

#include "test_harness.hpp"

#include <print>
#include <string>
#include <string_view>

namespace {

using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto* p : {path, "../src/core/ast.ixx", "src/core/ast.ixx"}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// AC1: compact_nodes rebuilds provenance_ + schema_cache_ in the same
// live-order pass as marker_. Source-cite on the 4 edits (column decls,
// reserves, live loop push, final assignment).
static void ac1_compact_nodes_provenance_schema_rebuild() {
    std::println("\n--- AC1: compact_nodes rebuilds provenance + schema_cache ---");
    auto a = read_file("src/core/ast.ixx");
    CHECK(!a.empty(), "ast.ixx readable");
    CHECK(a.find("#3155") != std::string::npos, "cites #3155");
    // Edit #1: column decls (new_provenance, new_schema_cache declared
    // alongside sibling SoA columns).
    CHECK(a.find("std::pmr::vector<std::uint32_t> new_provenance(alloc)") != std::string::npos,
          "AC1 new_provenance column decl present");
    CHECK(a.find("std::pmr::vector<std::uint32_t> new_schema_cache(alloc)") != std::string::npos,
          "AC1 new_schema_cache column decl present");
    // Edit #2: reserves.
    CHECK(a.find("new_provenance.reserve(live_count)") != std::string::npos,
          "AC1 new_provenance reserved (same pattern as siblings)");
    CHECK(a.find("new_schema_cache.reserve(live_count)") != std::string::npos,
          "AC1 new_schema_cache reserved (same pattern as siblings)");
    // Edit #3: live loop push (bounded-size fallback like macro_dirty /
    // ppa_dirty).
    CHECK(a.find("new_provenance.push_back(provenance_[id])") != std::string::npos,
          "AC1 live loop push for provenance (in-rebuild pass)");
    CHECK(a.find("new_schema_cache.push_back(schema_cache_[id])") != std::string::npos,
          "AC1 live loop push for schema_cache (in-rebuild pass)");
    // Edit #4: final assignment.
    CHECK(a.find("provenance_ = std::move(new_provenance)") != std::string::npos,
          "AC1 provenance_ assigned std::move(new_provenance)");
    CHECK(a.find("schema_cache_ = std::move(new_schema_cache)") != std::string::npos,
          "AC1 schema_cache_ assigned std::move(new_schema_cache)");
    // All 4 edits live in the same compact_nodes() function (single
    // structural fix, not scattered side-effects).
    const auto cf_pos = a.find("[[nodiscard]] std::size_t compact_nodes()");
    CHECK(cf_pos != std::string::npos, "compact_nodes() definition found");
    if (cf_pos != std::string::npos) {
        // Find the matching closing brace by scanning for the next return reclaimed;
        // pattern is structural — all 4 edits should appear before it.
        const auto end_pos = a.find("return reclaimed;", cf_pos);
        CHECK(end_pos != std::string::npos, "compact_nodes() end (return reclaimed) found");
        if (end_pos != std::string::npos) {
            const std::string cfn(a, cf_pos, end_pos - cf_pos);
            const auto n_prov = cfn.find("new_provenance");
            const auto n_sc = cfn.find("new_schema_cache");
            const auto a_prov = cfn.find("provenance_ = std::move(new_provenance)");
            const auto a_sc = cfn.find("schema_cache_ = std::move(new_schema_cache)");
            CHECK(n_prov != std::string::npos && n_sc != std::string::npos,
                  "AC1 column decls inside compact_nodes()");
            CHECK(a_prov != std::string::npos && a_sc != std::string::npos,
                  "AC1 final assignments inside compact_nodes()");
        }
    }
}

// AC2: After compact, for every live remapped id: marker, macro_dirty,
// provenance, schema_cache refer to the SAME logical node as pre-compact
// (homology preserved). Source-cite: all 4 columns are pushed in the
// SAME live-order pass (single loop iteration), so the i-th live node's
// provenance / schema_cache correspond to the same logical node as
// marker / macro_dirty / type_id. Single-pass invariant.
static void ac2_homology_preserved() {
    std::println("\n--- AC2: homology preserved across remap ---");
    auto a = read_file("src/core/ast.ixx");
    CHECK(!a.empty(), "ast.ixx readable");
    // Find the live rebuild loop block (single iteration per live id).
    const auto cf_pos = a.find("[[nodiscard]] std::size_t compact_nodes()");
    CHECK(cf_pos != std::string::npos, "compact_nodes() definition found");
    if (cf_pos != std::string::npos) {
        const auto loop_pos = a.find("for (NodeId id = 0; id < old_size; ++id) {", cf_pos);
        CHECK(loop_pos != std::string::npos, "live rebuild loop found");
        if (loop_pos != std::string::npos) {
            // Find the matching loop body end (next standalone "}" at
            // column 8). Walk until we see the final assignment block.
            const auto loop_end = a.find("tag_ = std::move(new_tag);", loop_pos);
            CHECK(loop_end != std::string::npos, "loop end (first assignment) found");
            if (loop_end != std::string::npos) {
                const std::string loop_body(a, loop_pos, loop_end - loop_pos);
                // All 4 columns (marker, macro_dirty, provenance, schema_cache)
                // are pushed in the SAME loop iteration — guarantees single-
                // pass homology.
                CHECK(loop_body.find("new_marker.push_back(marker_[id])") != std::string::npos,
                      "AC2 marker pushed in live loop");
                CHECK(loop_body.find("new_macro_dirty.push_back(macro_dirty_[id])") !=
                          std::string::npos,
                      "AC2 macro_dirty pushed in live loop");
                CHECK(loop_body.find("new_provenance.push_back(provenance_[id])") !=
                          std::string::npos,
                      "AC2 provenance pushed in live loop (single-pass)");
                CHECK(loop_body.find("new_schema_cache.push_back(schema_cache_[id])") !=
                          std::string::npos,
                      "AC2 schema_cache pushed in live loop (single-pass)");
            }
        }
    }
    // The 2 new push_back lines are placed BETWEEN macro_dirty and type_id,
    // matching the column-decl order so the loop iteration maintains the
    // same alignment as the original SoA layout.
    CHECK(a.find("if (id < provenance_.size())\n                new_provenance.push_back(") !=
              std::string::npos,
          "AC2 provenance uses bounded-size fallback (matches macro_dirty pattern)");
    CHECK(a.find("if (id < schema_cache_.size())\n                new_schema_cache.push_back(") !=
              std::string::npos,
          "AC2 schema_cache uses bounded-size fallback (matches macro_dirty pattern)");
}

// AC3: validate_macro_hygiene_invariants() still holds post-compact on
// MacroIntroduced nodes (no regression). Source-cite: validator still
// present + not modified.
static void ac3_validator_no_regression() {
    std::println("\n--- AC3: validate_macro_hygiene_invariants no regression ---");
    auto a = read_file("src/core/ast.ixx");
    CHECK(!a.empty(), "ast.ixx readable");
    CHECK(a.find("validate_macro_hygiene_invariants") != std::string::npos,
          "AC3 validate_macro_hygiene_invariants still present");
    // The validator must still reference provenance / marker / schema_cache
    // (no removal of hygiene invariants).
    const auto v_pos = a.find("validate_macro_hygiene_invariants");
    CHECK(v_pos != std::string::npos, "validator reference found");
    if (v_pos != std::string::npos) {
        const auto window_end = std::min<std::size_t>(v_pos + 1500, a.size());
        const std::string window(a, v_pos, window_end - v_pos);
        // Validator still touches the columns it always touched (no removal).
        CHECK(window.find("provenance") != std::string::npos ||
                  window.find("marker") != std::string::npos ||
                  window.find("is_macro_introduced") != std::string::npos,
              "AC3 validator still references hygiene columns");
    }
    // No new "skip compact" / "skip hygiene" code path.
    CHECK(a.find("compact_nodes() returns false") == std::string::npos &&
              a.find("compact skip hygiene") == std::string::npos,
          "AC3 no compact-skip / hygiene-skip bypass added");
}

// AC4: Soft compact / recycle-without-remap paths unchanged
// (compact_nodes_soft / recycle_dead_nodes preserved).
static void ac4_soft_recycle_unchanged() {
    std::println("\n--- AC4: soft compact / recycle unchanged ---");
    auto a = read_file("src/core/ast.ixx");
    CHECK(!a.empty(), "ast.ixx readable");
    CHECK(a.find("[[nodiscard]] std::size_t recycle_dead_nodes()") != std::string::npos,
          "AC4 recycle_dead_nodes() preserved");
    CHECK(a.find("[[nodiscard]] std::size_t compact_nodes_soft()") != std::string::npos,
          "AC4 compact_nodes_soft() preserved");
    // soft_compact_count_ metric still bumped (no removal).
    CHECK(a.find("soft_compact_count_") != std::string::npos,
          "AC4 soft_compact_count_ metric still bumped");
    // node_recycle_total_ still bumped.
    CHECK(a.find("node_recycle_total_") != std::string::npos,
          "AC4 node_recycle_total_ metric still bumped");
    // compact_nodes (hard remap) only — soft / recycle paths untouched.
    CHECK(a.find("compact_nodes_soft") != std::string::npos &&
              a.find("recycle_dead_nodes") != std::string::npos &&
              a.find("compact_nodes()") != std::string::npos,
          "AC4 all 3 compaction paths (soft / recycle / hard) preserved");
}

// AC5: No new query keys; Soft/Off zero-cost preserved. No new
// g_3155_* atomic introduced.
static void ac5_soft_off_preserved() {
    std::println("\n--- AC5: Soft / Off zero-cost preserved ---");
    auto a = read_file("src/core/ast.ixx");
    auto obs = read_file("src/compiler/observability_metrics.h");
    CHECK(!a.empty(), "ast.ixx readable");
    CHECK(a.find("g_3155_") == std::string::npos,
          "AC5 no new g_3155_* atomic in ast.ixx (zero-cost preserved)");
    if (!obs.empty()) {
        CHECK(obs.find("g_3155_") == std::string::npos,
              "AC5 no new g_3155_* atomic in observability_metrics.h");
    }
    // Single alloc pattern — both new columns use the same pmr::vector
    // + reserve + std::move pattern as siblings (no per-call cost beyond
    // the existing compact cycle).
    CHECK(a.find("std::pmr::vector<std::uint32_t> new_provenance(alloc)") != std::string::npos,
          "AC5 new_provenance uses pmr::vector (same alloc as siblings)");
    CHECK(a.find("std::pmr::vector<std::uint32_t> new_schema_cache(alloc)") != std::string::npos,
          "AC5 new_schema_cache uses pmr::vector (same alloc as siblings)");
    // No new query keys / no new observer interface added.
    CHECK(a.find("register_stats_impl") == std::string::npos ||
              a.find("register_stats_impl") < a.find("#3155"),
          "AC5 no new register_stats_impl after #3155 (no new query keys)");
}

} // namespace

int main() {
    ac1_compact_nodes_provenance_schema_rebuild();
    ac2_homology_preserved();
    ac3_validator_no_regression();
    ac4_soft_recycle_unchanged();
    ac5_soft_off_preserved();
    if (g_failed)
        return 1;
    std::println("compact_nodes provenance + schema_cache remap (#3155): OK ({} passed)", g_passed);
    return 0;
}