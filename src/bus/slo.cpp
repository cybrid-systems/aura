// ──────────────────────────────────────────────────────────────
//  slo.cpp — the bus's own performance and capacity counters (#5395)
// ──────────────────────────────────────────────────────────────
#include "bus/slo.hh"

#include <ctime>

namespace aura::bus {

std::uint64_t slo_thread_cpu_ns() {
    timespec ts{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

void SloHistogram::add(std::uint64_t ns) {
    ++count_;
    std::size_t bucket = 0;
    std::uint64_t value = ns;
    while (value > 1 && bucket + 1 < buckets_.size()) {
        value >>= 1;
        ++bucket;
    }
    ++buckets_[bucket];
}

std::uint64_t SloHistogram::p99() const {
    if (count_ == 0)
        return 0;
    // The 99th percentile: the bucket the ceil(0.99 * count) sample falls
    // into, reported as the bucket's upper bound.
    const std::uint64_t target = (count_ * 99 + 99) / 100; // ceil(0.99 n)
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
        seen += buckets_[i];
        if (seen >= target) {
            // The upper bound of the bucket: 2^(i+1) - 1 nanoseconds.
            return (1ull << (i + 1)) - 1;
        }
    }
    return (1ull << buckets_.size()) - 1;
}

void SloHistogram::reset() {
    buckets_.fill(0);
    count_ = 0;
}

} // namespace aura::bus
