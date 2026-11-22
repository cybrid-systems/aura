// ──────────────────────────────────────────────────────────────
//  degrade.cpp — the host's degradation detector (#5394)
// ──────────────────────────────────────────────────────────────
#include "bus/degrade.hh"

#include <algorithm>

namespace aura::bus {
namespace {

    constexpr std::int64_t kPpm = 1000000;

} // namespace

void DegradeDetector::configure(const DegradeConfig& config) {
    config_ = config;
    rebuild_states();
}

void DegradeDetector::install(const DegradeConfig& config) {
    history_.push_back(config_);
    const std::uint64_t previous = config_.version;
    config_ = config;
    // The detector owns the version: every install is the next one.
    config_.version = previous + 1;
    rebuild_states();
}

bool DegradeDetector::rollback() {
    if (history_.empty())
        return false;
    config_ = history_.back();
    history_.pop_back();
    rebuild_states();
    return true;
}

void DegradeDetector::rebuild_states() {
    states_.clear();
    states_.reserve(config_.keys.size());
    for (const DegradeKey& key : config_.keys) {
        DegradeKeyState state{};
        state.key = key.key;
        state.higher_is_worse = key.higher_is_worse;
        states_.push_back(state);
    }
    alerts_ = 0;
    samples_ = 0;
    last_change_ = 0;
}

void DegradeDetector::reset() {
    for (DegradeKeyState& state : states_) {
        state.samples = 0;
        state.alerts = 0;
        state.calm = 0;
        state.ewma = 0;
        state.scale = 1;
        state.cusum_ppm = 0;
    }
    alerts_ = 0;
    samples_ = 0;
    last_change_ = 0;
}

bool DegradeDetector::observe(std::uint64_t key, std::uint64_t value, std::uint64_t* fired_key) {
    if (config_.keys.empty())
        return false;
    ++samples_;
    const std::int64_t sample = static_cast<std::int64_t>(value);
    for (DegradeKeyState& state : states_) {
        if (state.key != key)
            continue;
        ++state.samples;
        const std::int64_t magnitude = sample < 0 ? -sample : sample;
        if (state.samples == 1) {
            // The first sample only initializes the level and the
            // scale: the warm-up is not a change.
            state.ewma = sample;
            state.scale = magnitude > 1 ? magnitude : 1;
            return false;
        }
        // The EWMA of the level and of the magnitude: the magnitude is
        // the normalizer, so the thresholds stay unit free.
        const std::int64_t alpha = static_cast<std::int64_t>(config_.ewma_alpha_ppm);
        state.ewma = state.ewma + ((sample - state.ewma) * alpha) / kPpm;
        state.scale = state.scale + ((magnitude - state.scale) * alpha) / kPpm;
        if (state.scale < 1)
            state.scale = 1;
        // The deviation from the smoothed level, in ppm of the scale.
        const std::int64_t deviation = ((sample - state.ewma) * kPpm) / state.scale;
        // The bad direction: a rise for a "higher is worse" metric, a
        // fall for the others.
        const std::int64_t bad = state.higher_is_worse ? std::max<std::int64_t>(deviation, 0)
                                                       : std::max<std::int64_t>(-deviation, 0);
        // The one-sided CUSUM: the bad deviation accumulates, the
        // drift gives it back, and it never goes below zero.
        state.cusum_ppm = std::max<std::int64_t>(
            0, state.cusum_ppm + bad - static_cast<std::int64_t>(config_.drift_ppm));
        // The calm counter advances while the change score has decayed
        // back to zero — the metric returned to its baseline. A
        // deviation does not reset it: the hysteresis must survive the
        // very deviation that follows an alert.
        if (state.cusum_ppm == 0)
            ++state.calm;
        // The alarm needs the minimum observation window (no alert on
        // the first samples) and, after an earlier alert, the
        // hysteresis: the metric must calm down first.
        if (state.samples < config_.min_samples)
            return false;
        if (state.cusum_ppm < static_cast<std::int64_t>(config_.limit_ppm))
            return false;
        if (state.alerts > 0 && state.calm < config_.calm_samples)
            return false;
        // Fire: the score resets, so the next alert needs a fresh
        // deviation.
        state.cusum_ppm = 0;
        state.calm = 0;
        ++state.alerts;
        ++alerts_;
        last_change_ = samples_;
        if (fired_key != nullptr)
            *fired_key = key;
        return true;
    }
    return false;
}

} // namespace aura::bus
