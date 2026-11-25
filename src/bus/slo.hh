// ──────────────────────────────────────────────────────────────
//  slo.hh — the bus's own performance and capacity counters (#5395)
//
//  The discipline is CPU time (#5324): on a shared runner the wall
//  clock measures the scheduler, not the work. The counters are the
//  coordinator thread's CPU time (CLOCK_THREAD_CPUTIME_ID), so they
//  measure what the bus itself costs — the overhead a peer pays.
//
//  The capacity plan answers the other half: how much room is left in
//  the image, and how many rounds that room still supports. The
//  coordinator warns before the room runs out, instead of failing a
//  publish halfway through a round.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_SLO_HH
#define AURA_BUS_SLO_HH

#include <array>
#include <cstdint>
#include <span>

namespace aura::bus {

// The coordinator thread's CPU time, in nanoseconds. Monotonic.
std::uint64_t slo_thread_cpu_ns();

// A log2 histogram over the per-frame CPU cost (bucket = log2 of the
// nanoseconds, capped). Sixty-four buckets cover a microsecond to
// hours.
class SloHistogram {
public:
    void add(std::uint64_t ns);
    // The p99 of the samples, as the upper bound of the bucket the 99th
    // percentile falls into. 0 when there is no sample.
    [[nodiscard]] std::uint64_t p99() const;
    [[nodiscard]] std::uint64_t count() const { return count_; }
    void reset();

private:
    std::array<std::uint64_t, 64> buckets_{};
    std::uint64_t count_ = 0;
};

// The performance counters.
struct SloCounters {
    std::uint64_t frames = 0;         // the control frames served
    std::uint64_t frame_cpu_ns = 0;   // the CPU time serving them
    std::uint64_t p99_cpu_ns = 0;     // the p99 of the per-frame CPU cost
    std::uint64_t publishes = 0;      // the records published
    std::uint64_t publish_cpu_ns = 0; // the CPU time publishing them
    std::uint64_t attaches = 0;       // the handshakes
    std::uint64_t attach_cpu_ns = 0;  // the CPU time of the handshakes
    std::uint64_t catchups = 0;       // the catchup replies
    std::uint64_t catchup_max = 0;    // the largest catchup increment
    std::uint64_t version = 1;
};

// The capacity plan: the room left in the image and the rounds it
// still supports.
struct CapacityPlan {
    std::uint64_t remaining[5] = {}; // metrics, cct, patches, worldlines, nodes
    std::uint64_t capacity[5] = {};
    std::uint64_t rounds = 0; // the rounds the room still supports
    std::uint64_t low = 0;    // 1 = the room is running low
    std::uint64_t committed_bytes = 0;
    std::uint64_t image_bytes = 0;
    // #5428: the string table is a separate, fixed wall: the source
    // carriers of a candidate loop fill it before any channel fills.
    std::uint64_t remaining_source_slots = 0;
    std::uint64_t remaining_source_bytes = 0;
    std::uint64_t version = 1;
};

// The rounds a round needs: one worldline record and two patch
// records. The estimate is deliberately conservative.
inline constexpr std::uint64_t kSloWorldlinesPerRound = 1;
inline constexpr std::uint64_t kSloPatchesPerRound = 2;
// #5428: one candidate source carrier per round.
inline constexpr std::uint64_t kSloSourcesPerRound = 1;
// #5468: a carrier is at most one control frame of text, and its bytes
// are 8-aligned. Charging the largest aligned carrier per round keeps
// the estimate conservative: the byte region is a wall of its own, not
// just the slot count.
inline constexpr std::uint64_t kSloSourceBytesPerRound = 4096;
// Below this many rounds the coordinator warns.
inline constexpr std::uint64_t kSloLowRounds = 8;

} // namespace aura::bus

#endif
