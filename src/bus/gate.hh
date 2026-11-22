// ──────────────────────────────────────────────────────────────
//  gate.hh — statistical admission contract for the bus (#5391)
//
//  A judge measures a candidate on held-out folds and binds the raw
//  evidence to its trial. The coordinator does not take the judge's
//  word for it: it recomputes the deflated statistic from the bound
//  windows, applies the multiple-comparison correction for the round
//  family it observed itself, and decides. A KEEP without a matching
//  statistical verdict is refused.
//
//  Every number is fixed point (parts per million), so the same
//  evidence always produces byte-identical verdicts: the replay of a
//  worldline recomputes the very same record.
//
//  The math mirrors std/validation (val:deflate): the mean and the
//  sample standard deviation of the held-out windows, the standard
//  error, and the Bailey/Lopez-de-Prado haircut
//  (E[max of N normals] * se) for the family size.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_GATE_HH
#define AURA_BUS_GATE_HH

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aura::bus {

// The contract version. Bumped when the wire body or the statistics
// change; the verdict record carries the policy version that decided
// it, so a replay can tell which rules produced a verdict.
inline constexpr std::uint32_t kGateContractVersion = 1;

// The held-out windows one candidate may carry.
inline constexpr std::size_t kGateMaxWindows = 16;
// The fewest held-out windows a usable evaluation has.
inline constexpr std::uint32_t kGateMinWindows = 3;
// Parts per million: the fixed-point scale of every metric delta.
inline constexpr std::int64_t kGatePpm = 1000000;
// The largest held-out window the contract accepts (parts per
// million). It bounds the mean, the standard error and the haircut, so
// every derived number stays inside the image's immediate domain.
inline constexpr std::int64_t kGateMaxWindowPpm = 1ll << 40;
// The bias that keeps a signed metric inside the image's immediate
// domain (bit 63 clear): stored = value + kGateBias.
inline constexpr std::uint64_t kGateBias = 1ull << 61;

enum class GateVerdict : std::uint8_t {
    Unknown = 0,
    Keep = 1,
    Reject = 2,
};

// The wire body of a GateBind frame: one candidate's held-out
// evidence. The windows are the per-fold out-of-sample metric deltas
// in parts per million (signed, two's complement in the u64 slot).
struct GateEvidenceBody {
    std::uint64_t worldline = 0;
    std::uint64_t round_id = 0;     // the admission round (the family)
    std::uint64_t workload_sym = 0; // must match the evidence five-tuple
    std::uint64_t seed = 0;
    std::uint64_t verifier_version = 0;
    std::uint64_t fold_count = 0;    // 1..kGateMaxWindows
    std::uint64_t threshold_ppm = 0; // the bar the judge applied
    std::uint64_t control = 0;       // 1 = the round's negative control
    std::uint64_t verdict = 0;       // the judge's own verdict (audited)
    std::uint64_t windows[kGateMaxWindows] = {};
};

// The canonical verdict. Trivially copyable so two runs compare with
// memcmp: the replay of a worldline must reproduce it byte for byte.
struct GateVerdictRecord {
    std::uint64_t verdict = 0; // GateVerdict
    std::uint64_t family = 0;  // the family size the correction used
    std::uint64_t windows = 0; // the held-out windows the verdict used
    std::int64_t observed_ppm = 0;
    std::int64_t adjusted_ppm = 0;
    std::uint64_t haircut_ppm = 0;
    std::uint64_t threshold_ppm = 0;
    std::uint64_t tighten_factor = 0;
    std::uint64_t policy_version = 0;
    std::uint64_t fingerprint = 0;
};

// The coordinator's admission policy. Tightening raises the haircut
// multiplier: it is scale free, so it bites whatever the metric is.
struct GatePolicy {
    bool required = false;           // enforce the statistical admission
    bool require_control = true;     // every round carries a negative control
    std::uint64_t threshold_ppm = 0; // the keep bar on the deflated mean
    std::uint32_t min_windows = kGateMinWindows;
    std::uint64_t negative_budget_ppm = 200000; // 20% negative KEEPs
    std::uint32_t budget_window = 8;            // the sliding window size
    std::uint64_t tighten_factor = 1;
    std::uint64_t version = 1;
};

// The sliding window of negative KEEP events. A negative event is a
// KEEP that was reverted, or a negative control the statistics
// accepted. Over the budget the coordinator tightens the admission.
class NegativeKeepWindow {
public:
    void configure(std::size_t size);
    // Record a fresh (positive) KEEP; returns its slot so a later
    // revert can mark it negative.
    std::size_t record_positive();
    // Record a negative event without a KEEP (a negative control the
    // statistics wrongly accepted).
    std::size_t record_negative();
    // Mark a recorded slot negative (a reverted KEEP).
    void mark_negative(std::size_t slot);
    [[nodiscard]] std::size_t total() const { return count_; }
    [[nodiscard]] std::size_t negatives() const;
    [[nodiscard]] std::uint64_t rate_ppm() const;
    [[nodiscard]] bool full() const { return count_ == slots_.size(); }
    // True when the window is full and the negative rate is over the
    // budget.
    [[nodiscard]] bool over_budget(std::uint64_t budget_ppm) const;
    void reset();

private:
    std::vector<char> slots_{}; // 0 = positive, 1 = negative
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

// FNV-1a 64 over the canonical evidence bytes (the five-tuple tie plus
// the windows). The coordinator computes it: a judge cannot claim a
// fingerprint it did not earn.
std::uint64_t gate_fingerprint(const GateEvidenceBody& evidence);

// The mean and the sample standard deviation of the held-out windows,
// in parts per million. False when the window count is out of range.
bool gate_window_stats(const std::uint64_t* windows, std::size_t count, std::int64_t* observed_ppm,
                       std::uint64_t* se_ppm);

// E[max of N independent standard normals] — the deflated-Sharpe
// haircut factor. 0 for one try, so a single candidate is uncorrected
// by construction and every extra candidate can only raise the bar.
double gate_z_expected_max(std::uint64_t trials);

// The admission decision for one candidate. `reason` names the
// outcome: "ok", "insufficient-windows" or "not-better".
GateVerdictRecord gate_admit(const GateEvidenceBody& evidence, std::uint64_t family,
                             const GatePolicy& policy, std::string* reason);

} // namespace aura::bus

#endif
