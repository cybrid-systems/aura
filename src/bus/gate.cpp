// ──────────────────────────────────────────────────────────────
//  gate.cpp — the statistical admission contract (#5391)
//
//  The coordinator recomputes the judge's numbers from the bound
//  windows, so a summary that flatters a candidate buys nothing.
// ──────────────────────────────────────────────────────────────
#include "bus/gate.hh"

#include <cmath>
#include <type_traits>

namespace aura::bus {
namespace {

    // Acklam's rational approximation of the normal quantile (the
    // same one std/validation's val:z-quantile uses, so the busified
    // admission and the EDSL deflation agree).
    constexpr double kZA[6] = {-39.69683028665376, 220.9460984245205,  -275.9285104469687,
                               138.3577518672690,  -30.66479806614716, 2.506628277459239};
    constexpr double kZB[5] = {-54.47609879812406, 161.5858368580409, -155.6989798598866,
                               66.80131188771972, -13.28068155288572};
    constexpr double kZC[6] = {-0.007784894002430293, -0.3223964580411365, -2.400758277161838,
                               -2.549732539343734,    4.374664141464968,   2.938163982698783};
    constexpr double kZD[4] = {0.007784695709041462, 0.3224671290700398, 2.445134137142996,
                               3.754408661907416};

    double poly(const double* cs, std::size_t n, double x) {
        double acc = 0.0;
        for (std::size_t i = 0; i < n; ++i)
            acc = acc * x + cs[i];
        return acc;
    }

    double z_quantile(double p) {
        if (p < 0.000000000001)
            p = 0.000000000001;
        else if (p > 0.999999999999)
            p = 0.999999999999;
        if (p < 0.02425) {
            const double q = std::sqrt(-2.0 * std::log(p));
            return poly(kZC, 6, q) / (poly(kZD, 4, q) * q + 1.0);
        }
        if (p <= 0.97575) {
            const double q = p - 0.5;
            const double r = q * q;
            return poly(kZA, 6, r) * q / (poly(kZB, 5, r) * r + 1.0);
        }
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        return -(poly(kZC, 6, q) / (poly(kZD, 4, q) * q + 1.0));
    }

    void hash_u64(std::uint64_t& h, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xffu;
            h *= 1099511628211ull;
        }
    }

} // namespace

static_assert(std::is_trivially_copyable_v<GateVerdictRecord>,
              "the verdict record is compared byte for byte");

double gate_z_expected_max(std::uint64_t trials) {
    if (trials <= 1)
        return 0.0;
    const double k = static_cast<double>(trials);
    const double gamma = 0.5772156649015329;
    const double e = 2.718281828459045;
    return (1.0 - gamma) * z_quantile(1.0 - 1.0 / k) + gamma * z_quantile(1.0 - 1.0 / (k * e));
}

std::uint64_t gate_fingerprint(const GateEvidenceBody& evidence) {
    std::uint64_t h = 14695981039346656037ull;
    hash_u64(h, evidence.worldline);
    hash_u64(h, evidence.round_id);
    hash_u64(h, evidence.workload_sym);
    hash_u64(h, evidence.seed);
    hash_u64(h, evidence.verifier_version);
    hash_u64(h, evidence.fold_count);
    hash_u64(h, evidence.threshold_ppm);
    hash_u64(h, evidence.control);
    const std::size_t n = evidence.fold_count <= kGateMaxWindows
                              ? static_cast<std::size_t>(evidence.fold_count)
                              : kGateMaxWindows;
    for (std::size_t i = 0; i < n; ++i)
        hash_u64(h, evidence.windows[i]);
    return h;
}

bool gate_window_stats(const std::uint64_t* windows, std::size_t count, std::int64_t* observed_ppm,
                       std::uint64_t* se_ppm) {
    if (windows == nullptr || count < 2 || count > kGateMaxWindows)
        return false;
    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i)
        sum += static_cast<double>(static_cast<std::int64_t>(windows[i]));
    const double mean = sum / static_cast<double>(count);
    double ss = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double d = static_cast<double>(static_cast<std::int64_t>(windows[i])) - mean;
        ss += d * d;
    }
    const double sd = std::sqrt(ss / static_cast<double>(count - 1));
    const double se = sd / std::sqrt(static_cast<double>(count));
    if (observed_ppm != nullptr)
        *observed_ppm = static_cast<std::int64_t>(std::llround(mean));
    if (se_ppm != nullptr)
        *se_ppm = static_cast<std::uint64_t>(std::llround(se));
    return true;
}

GateVerdictRecord gate_admit(const GateEvidenceBody& evidence, std::uint64_t family,
                             const GatePolicy& policy, std::string* reason) {
    GateVerdictRecord rec{};
    rec.family = family;
    rec.windows = evidence.fold_count;
    rec.threshold_ppm = policy.threshold_ppm;
    rec.tighten_factor = policy.tighten_factor;
    rec.policy_version = policy.version;
    rec.fingerprint = gate_fingerprint(evidence);
    const std::size_t n = static_cast<std::size_t>(evidence.fold_count);
    if (n < policy.min_windows || n > kGateMaxWindows) {
        rec.verdict = static_cast<std::uint64_t>(GateVerdict::Reject);
        if (reason != nullptr)
            *reason = "insufficient-windows";
        return rec;
    }
    std::int64_t observed = 0;
    std::uint64_t se = 0;
    if (!gate_window_stats(evidence.windows, n, &observed, &se)) {
        rec.verdict = static_cast<std::uint64_t>(GateVerdict::Reject);
        if (reason != nullptr)
            *reason = "insufficient-windows";
        return rec;
    }
    rec.observed_ppm = observed;
    const double haircut = gate_z_expected_max(family) * static_cast<double>(se) *
                           static_cast<double>(policy.tighten_factor);
    std::int64_t haircut_ppm = static_cast<std::int64_t>(std::llround(haircut));
    if (haircut_ppm < 0)
        haircut_ppm = 0;
    if (haircut_ppm > kGateMaxWindowPpm * 64)
        haircut_ppm = kGateMaxWindowPpm * 64;
    rec.haircut_ppm = static_cast<std::uint64_t>(haircut_ppm);
    rec.adjusted_ppm = observed - haircut_ppm;
    const bool keep = rec.adjusted_ppm > static_cast<std::int64_t>(policy.threshold_ppm);
    rec.verdict = static_cast<std::uint64_t>(keep ? GateVerdict::Keep : GateVerdict::Reject);
    if (reason != nullptr)
        *reason = keep ? "ok" : "not-better";
    return rec;
}

void NegativeKeepWindow::configure(std::size_t size) {
    slots_.assign(size == 0 ? 1 : size, 0);
    next_ = 0;
    count_ = 0;
}

std::size_t NegativeKeepWindow::record_positive() {
    if (slots_.empty())
        configure(1);
    slots_[next_] = 0;
    const std::size_t slot = next_;
    next_ = (next_ + 1) % slots_.size();
    if (count_ < slots_.size())
        ++count_;
    return slot;
}

std::size_t NegativeKeepWindow::record_negative() {
    const std::size_t slot = record_positive();
    slots_[slot] = 1;
    return slot;
}

void NegativeKeepWindow::mark_negative(std::size_t slot) {
    if (slot < slots_.size())
        slots_[slot] = 1;
}

std::size_t NegativeKeepWindow::negatives() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < count_ && i < slots_.size(); ++i)
        n += slots_[i] != 0 ? 1 : 0;
    return n;
}

std::uint64_t NegativeKeepWindow::rate_ppm() const {
    if (count_ == 0)
        return 0;
    return static_cast<std::uint64_t>(negatives()) * static_cast<std::uint64_t>(kGatePpm) /
           static_cast<std::uint64_t>(count_);
}

bool NegativeKeepWindow::over_budget(std::uint64_t budget_ppm) const {
    return full() && rate_ppm() > budget_ppm;
}

void NegativeKeepWindow::reset() {
    std::fill(slots_.begin(), slots_.end(), 0);
    next_ = 0;
    count_ = 0;
}

} // namespace aura::bus
