// ──────────────────────────────────────────────────────────────
//  degrade.hh — the host's degradation detector (#5394)
//
//  The host watches a few key metrics on the metrics channel and runs
//  an EWMA plus a one-sided CUSUM change test on them. A sustained
//  degradation opens a round (the signal state of the round machine);
//  the canary observation window reuses the same detector, so a
//  candidate that degrades the metric is rolled back.
//
//  The detection is relative: the CUSUM counts the deviation in the
//  bad direction, normalized by the metric's own scale, so the
//  thresholds are unit free (parts per million of the metric's
//  magnitude). Everything is integer arithmetic: the same samples
//  always give the same verdict.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_DEGRADE_HH
#define AURA_BUS_DEGRADE_HH

#include <cstdint>
#include <string>
#include <vector>

namespace aura::bus {

// One watched metric.
struct DegradeKey {
    std::uint64_t key = 0;
    bool higher_is_worse = false;
};

// The detector's configuration. Versioned: installing a new one bumps
// the version, a rollback restores the previous one.
struct DegradeConfig {
    std::vector<DegradeKey> keys{};
    std::uint64_t min_samples = 8;         // the minimum observation window
    std::uint64_t ewma_alpha_ppm = 250000; // the EWMA smoothing (ppm of 1.0)
    std::uint64_t drift_ppm = 20000;       // the CUSUM drift (ppm per sample)
    std::uint64_t limit_ppm = 200000;      // the CUSUM decision interval
    std::uint64_t calm_samples = 4;        // the hysteresis: calm samples before a new alert
    std::uint64_t version = 1;
};

// The per-key state.
struct DegradeKeyState {
    std::uint64_t key = 0;
    bool higher_is_worse = false;
    std::uint64_t samples = 0;
    std::uint64_t alerts = 0;
    std::uint64_t calm = 0;     // the hysteresis counter
    std::int64_t ewma = 0;      // the smoothed level (the metric's units)
    std::int64_t scale = 1;     // the smoothed magnitude (the normalizer)
    std::int64_t cusum_ppm = 0; // the change score
};

class DegradeDetector {
public:
    void configure(const DegradeConfig& config);
    // Install a new configuration (a new version). The previous one
    // stays available for a rollback.
    void install(const DegradeConfig& config);
    // Restore the previous configuration. False when there is none.
    bool rollback();
    [[nodiscard]] const DegradeConfig& config() const { return config_; }
    [[nodiscard]] std::uint64_t version() const { return config_.version; }
    [[nodiscard]] std::uint64_t alerts() const { return alerts_; }
    [[nodiscard]] std::uint64_t samples() const { return samples_; }
    // The sample index of the last alert (0 = none).
    [[nodiscard]] std::uint64_t last_change() const { return last_change_; }
    [[nodiscard]] const std::vector<DegradeKeyState>& states() const { return states_; }
    // Observe one sample. Returns true when the detector fires (a
    // sustained degradation); `fired_key` names the metric.
    bool observe(std::uint64_t key, std::uint64_t value, std::uint64_t* fired_key = nullptr);
    void reset();

private:
    void rebuild_states();

    DegradeConfig config_{};
    std::vector<DegradeConfig> history_{};
    std::vector<DegradeKeyState> states_{};
    std::uint64_t alerts_ = 0;
    std::uint64_t samples_ = 0;
    std::uint64_t last_change_ = 0;
};

} // namespace aura::bus

#endif
