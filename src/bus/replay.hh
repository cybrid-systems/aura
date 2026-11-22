// ──────────────────────────────────────────────────────────────
//  replay.hh — audit replay and deterministic reproduction (#5392)
//
//  The closed loop must be provable after the fact: the audit log
//  and the image must reconstruct the same lineage, and every KEEP
//  must carry the evidence that let it through. The replay checks
//  that, and reproduces an admission verdict from the bound
//  evidence — byte for byte, twice in a row.
//
//  The replay is a read: it never changes the workspace, the ledger
//  or the log.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_REPLAY_HH
#define AURA_BUS_REPLAY_HH

#include "bus/capability.hh"
#include "bus/gate.hh"
#include "bus/image_format.hh"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace aura::bus {

// What a replay found. Zero is "fine"; every other value names one
// defect, and the report keeps the first one with its sequence.
enum class ReplayFinding : std::uint8_t {
    Ok = 0,
    OrphanEvent = 1,           // an event names a worldline that was never opened
    BrokenParent = 2,          // a worldline record's parent does not resolve
    SeqRegression = 3,         // the image audit section's sequence goes backwards
    AuditMismatch = 4,         // the image audit section and the control log disagree
    KeepWithoutConstraint = 5, // a KEEP without a passing constraint result
    KeepWithoutVerdict = 6,    // a KEEP without an admission verdict
    VerdictMismatch = 7,       // the recomputed verdict differs from the recorded one
    EvidenceMismatch = 8,      // the bound evidence does not match the five-tuple
};

// One worldline's lineage.
struct ReplayNode {
    std::uint64_t worldline = 0;
    std::uint64_t parent = 0;      // the parent worldline (0 = none)
    std::uint64_t state = 0;       // the final WorldlineState
    std::uint64_t trial_seq = 0;   // the audit sequence of the Trial event
    std::uint64_t keep_seq = 0;    // the audit sequence of the Keep event (0 = none)
    std::uint64_t verdict_seq = 0; // the audit sequence of the GateVerdict event
    std::uint64_t constraint = 0;  // the ConstraintResult of the last record
    std::uint64_t reproduced = 0;  // 1 = the recomputed verdict matches
    std::uint64_t fingerprint = 0; // the evidence fingerprint
};

// One worldline's evidence, as the coordinator holds it.
struct ReplayEvidence {
    std::uint64_t worldline = 0;
    // The evidence five-tuple (#5379), spelled out so this header
    // stays independent of the wire protocol.
    std::uint64_t baseline_gen = 0;
    std::uint64_t candidate_hash = 0;
    std::uint64_t workload_sym = 0;
    std::uint64_t seed = 0;
    std::uint64_t verifier_version = 0;
    GateEvidenceBody gate{};
    GateVerdictRecord verdict{};
    bool has_five = false;
    bool has_gate = false;
    bool has_verdict = false;
};

struct ReplayReport {
    std::uint64_t events = 0;      // the audit events inspected
    std::uint64_t worldlines = 0;  // the worldlines in the lineage
    std::uint64_t keeps = 0;       // the KEEP events inspected
    std::uint64_t findings = 0;    // the defects found
    std::uint64_t first_seq = 0;   // the audit sequence of the first defect
    std::uint64_t first_kind = 0;  // the ReplayFinding of the first defect
    std::string first_reason;      // what the first defect was
    std::vector<ReplayNode> nodes; // the lineage, in trial order

    [[nodiscard]] bool ok() const { return findings == 0; }
};

// Reconstruct the lineage from the control-plane log and the image,
// check its integrity and reproduce the admission verdicts.
//
// `image` may be empty (no mapping): the audit-only checks still run,
// the image-backed ones do not. `candidate_digest` recomputes a
// trial's patch digest; when it is given, the recorded candidate hash
// is checked against it.
ReplayReport replay_lineage(const std::vector<AuditEvent>& events, std::span<const std::byte> image,
                            const std::vector<ReplayEvidence>& evidence, bool gate_required,
                            const std::function<std::uint64_t(std::uint64_t)>& candidate_digest);

} // namespace aura::bus

#endif
