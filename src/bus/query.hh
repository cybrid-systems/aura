// ──────────────────────────────────────────────────────────────
//  query.hh — agent query face over a committed image (#5125)
//
//  Aggregations are reader-side: the client already maps the image, so
//  hotspots and worldline diffs decode committed records without a
//  control frame. No allocation on the image path beyond the result.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_QUERY_HH
#define AURA_BUS_QUERY_HH

#include "bus/image_format.hh"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aura::bus {

struct Hotspot {
    std::uint64_t mid = 0;
    std::uint64_t samples = 0;
    std::uint64_t self_ns = 0;
    std::uint64_t node = kImageNoIndex;
};

// Aggregate the CCT by mid_lo, descending by self_ns then samples.
bool cct_hotspots(std::span<const std::byte> image, std::size_t top, std::vector<Hotspot>* out,
                  std::string* why);

struct PatchDiff {
    std::uint64_t node = kImageNoIndex;
    bool in_a = false;
    bool in_b = false;
    std::uint32_t field_a = 0;
    std::uint32_t field_b = 0;
    std::uint64_t value_a = 0;
    std::uint64_t value_b = 0;
};

struct WorldlineDiff {
    std::uint8_t state_a = 0;
    std::uint8_t state_b = 0;
    std::uint8_t constraint_a = 0;
    std::uint8_t constraint_b = 0;
    std::uint64_t metrics_a = kImageNoIndex;
    std::uint64_t metrics_b = kImageNoIndex;
    std::vector<PatchDiff> rows; // union of patched nodes that differ
};

// Compare two worldlines by their real ids (first match in the table).
// Patch rows are the union of both patch ranges; identical rows are
// dropped. Returns false when the section or either worldline is absent.
bool worldline_diff(std::span<const std::byte> image, std::uint64_t id_a, std::uint64_t id_b,
                    WorldlineDiff* out, std::string* why);

} // namespace aura::bus

#endif
