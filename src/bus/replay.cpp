// ──────────────────────────────────────────────────────────────
//  replay.cpp — audit replay and deterministic reproduction (#5392)
// ──────────────────────────────────────────────────────────────
#include "bus/replay.hh"

#include "bus/image.hh"

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>

namespace aura::bus {
namespace {

    // The audit events that name a worldline must follow its Trial.
    bool names_worldline(AuditKind kind) {
        switch (kind) {
            case AuditKind::Trial:
            case AuditKind::Keep:
            case AuditKind::Rollback:
            case AuditKind::CanaryStart:
            case AuditKind::CanarySeal:
            case AuditKind::CanaryRevert:
            case AuditKind::GateBind:
            case AuditKind::GateVerdict:
            case AuditKind::Denied:
                return true;
            default:
                return false;
        }
    }

    struct Collector {
        ReplayReport* report = nullptr;
        void add(ReplayFinding kind, std::uint64_t seq, std::string reason) {
            ++report->findings;
            if (report->findings == 1) {
                report->first_seq = seq;
                report->first_kind = static_cast<std::uint64_t>(kind);
                report->first_reason = std::move(reason);
            }
        }
    };

} // namespace

ReplayReport replay_lineage(const std::vector<AuditEvent>& events, std::span<const std::byte> image,
                            const std::vector<ReplayEvidence>& evidence, bool gate_required,
                            const std::function<std::uint64_t(std::uint64_t)>& candidate_digest) {
    ReplayReport report{};
    report.events = events.size();
    Collector sink{&report};

    // ── The worldline records of the image ──────────────────────
    std::vector<WorldlineRecord> records;
    std::unordered_map<std::uint64_t, const WorldlineRecord*> by_id;
    if (!image.empty()) {
        SectionEntry se{};
        if (find_section(image, SectionKind::Worldlines, &se) && se.element_size != 0 &&
            se.count <= kImageMaxRecords && se.length % se.element_size == 0 &&
            se.offset + se.length <= image.size()) {
            const std::uint64_t stride = se.element_size;
            records.reserve(se.count);
            for (std::uint64_t i = 0; i < se.count; ++i) {
                WorldlineRecord w{};
                std::memcpy(&w, image.data() + se.offset + i * stride,
                            static_cast<std::size_t>(std::min<std::uint64_t>(stride, sizeof(w))));
                records.push_back(w);
            }
            // The pointers are taken after the vector stops growing.
            for (const WorldlineRecord& w : records)
                by_id[w.id] = &w;
        }
    }

    // ── The image audit section against the control-plane log ──
    if (!image.empty()) {
        SectionEntry se{};
        if (find_section(image, SectionKind::Audit, &se) &&
            se.element_size == sizeof(AuditRecord) && se.count <= kImageMaxRecords &&
            se.length % se.element_size == 0 && se.offset + se.length <= image.size()) {
            std::uint64_t previous = 0;
            for (std::uint64_t i = 0; i < se.count; ++i) {
                AuditRecord rec{};
                std::memcpy(&rec, image.data() + se.offset + i * se.element_size, sizeof(rec));
                if (rec.seq <= previous) {
                    sink.add(ReplayFinding::SeqRegression, rec.seq,
                             "the image audit sequence goes backwards at " +
                                 std::to_string(rec.seq));
                    break;
                }
                previous = rec.seq;
                // The image mirrors the control-plane log: the same
                // event, the same worldline.
                if (rec.seq >= 1 && rec.seq <= events.size()) {
                    const AuditEvent& event = events[rec.seq - 1];
                    if (event.kind != static_cast<AuditKind>(rec.kind) ||
                        event.worldline != rec.target) {
                        sink.add(ReplayFinding::AuditMismatch, rec.seq,
                                 "the image audit record " + std::to_string(rec.seq) +
                                     " and the control log disagree");
                        break;
                    }
                }
            }
        }
    }

    // ── The lineage from the control-plane log ─────────────────
    std::unordered_map<std::uint64_t, ReplayNode> nodes;
    std::unordered_map<std::uint64_t, std::uint64_t> opened; // worldline -> trial seq
    for (std::size_t i = 0; i < events.size(); ++i) {
        const AuditEvent& event = events[i];
        const std::uint64_t seq = static_cast<std::uint64_t>(i) + 1;
        if (!names_worldline(event.kind) || event.worldline == 0)
            continue;
        if (event.kind == AuditKind::Trial) {
            opened[event.worldline] = seq;
            ReplayNode& node = nodes[event.worldline];
            node.worldline = event.worldline;
            node.trial_seq = seq;
            continue;
        }
        const auto it = opened.find(event.worldline);
        if (it == opened.end()) {
            sink.add(ReplayFinding::OrphanEvent, seq,
                     "the event names worldline " + std::to_string(event.worldline) +
                         ", which was never opened");
            continue;
        }
        ReplayNode& node = nodes[event.worldline];
        node.worldline = event.worldline;
        switch (event.kind) {
            case AuditKind::Keep:
                node.keep_seq = seq;
                ++report.keeps;
                break;
            case AuditKind::GateVerdict:
                node.verdict_seq = seq;
                break;
            default:
                break;
        }
    }

    // The records carry the final state, the constraint result and the
    // published evidence tails.
    for (const auto& [id, record] : by_id) {
        ReplayNode& node = nodes[id];
        node.worldline = id;
        node.parent = record->parent == kImageNoParent ? 0 : record->parent;
        node.state = record->state;
        node.constraint = record->constraint_result;
        // #5424: an image written before the fingerprint was truncated
        // may still carry a raw 64-bit FNV value. The replay node feeds
        // an Aura immediate, so mask it here as well as at the source.
        node.fingerprint = record->gate_fingerprint & ((1ull << 62) - 1);
        if (node.parent != 0 && by_id.find(node.parent) == by_id.end() &&
            opened.find(node.parent) == opened.end()) {
            sink.add(ReplayFinding::BrokenParent, node.trial_seq,
                     "worldline " + std::to_string(id) + " names the unknown parent " +
                         std::to_string(node.parent));
        }
    }

    // ── The evidence: the five-tuple, the admission, the replay ──
    for (const ReplayEvidence& item : evidence) {
        ReplayNode& node = nodes[item.worldline];
        node.worldline = item.worldline;
        // Evidence for a worldline that was never opened is an orphan.
        if (opened.find(item.worldline) == opened.end()) {
            sink.add(ReplayFinding::OrphanEvent, node.trial_seq,
                     "the evidence names worldline " + std::to_string(item.worldline) +
                         ", which was never opened");
        }
        if (item.has_gate && item.has_verdict) {
            // The evidence must belong to the same evaluation as the
            // five-tuple.
            if (item.has_five &&
                (item.workload_sym != item.gate.workload_sym || item.seed != item.gate.seed ||
                 item.verifier_version != item.gate.verifier_version)) {
                sink.add(ReplayFinding::EvidenceMismatch, node.verdict_seq,
                         "the statistical evidence of worldline " + std::to_string(item.worldline) +
                             " does not match its five-tuple");
            }
            // The candidate hash must still match the trial's patch
            // bytes: a tampered hash breaks the reproduction.
            if (item.has_five && candidate_digest) {
                const std::uint64_t digest = candidate_digest(item.worldline);
                if ((digest & ((1ull << 62) - 1)) != (item.candidate_hash & ((1ull << 62) - 1))) {
                    sink.add(ReplayFinding::EvidenceMismatch, node.verdict_seq,
                             "the candidate hash of worldline " + std::to_string(item.worldline) +
                                 " does not match its patch bytes");
                }
            }
            // The reproduction: recompute the verdict from the bound
            // evidence with the policy the verdict was made under.
            GatePolicy policy{};
            policy.threshold_ppm = item.verdict.threshold_ppm;
            policy.min_windows = static_cast<std::uint32_t>(item.verdict.min_windows);
            policy.tighten_factor = item.verdict.tighten_factor;
            policy.version = item.verdict.policy_version;
            std::string reason;
            const GateVerdictRecord recomputed =
                gate_admit(item.gate, item.verdict.family, policy, &reason);
            if (std::memcmp(&recomputed, &item.verdict, sizeof(recomputed)) != 0) {
                sink.add(ReplayFinding::VerdictMismatch, node.verdict_seq,
                         "the recomputed verdict of worldline " + std::to_string(item.worldline) +
                             " differs from the recorded one");
                node.reproduced = 0;
            } else {
                node.reproduced = 1;
            }
            // The published tail must agree with the verdict.
            const auto it = by_id.find(item.worldline);
            if (it != by_id.end()) {
                const WorldlineRecord& record = *it->second;
                const std::int64_t adjusted = static_cast<std::int64_t>(record.gate_adjusted_ppm) -
                                              static_cast<std::int64_t>(kGateBias);
                if (record.gate_verdict != item.verdict.verdict ||
                    record.gate_family != item.verdict.family ||
                    adjusted != item.verdict.adjusted_ppm ||
                    record.gate_threshold_ppm != item.verdict.threshold_ppm ||
                    record.gate_fingerprint != item.verdict.fingerprint) {
                    sink.add(ReplayFinding::VerdictMismatch, node.verdict_seq,
                             "the published verdict of worldline " +
                                 std::to_string(item.worldline) + " differs from the recorded one");
                    node.reproduced = 0;
                }
            }
        }
        // A KEEP needs a verdict while the gate is on.
        if (gate_required && node.keep_seq != 0 && !item.has_verdict) {
            sink.add(ReplayFinding::KeepWithoutVerdict, node.keep_seq,
                     "worldline " + std::to_string(item.worldline) +
                         " was kept without an admission verdict");
        }
        // A KEEP needs a passing constraint result.
        if (node.keep_seq != 0 && node.constraint != 0 &&
            node.constraint != static_cast<std::uint64_t>(ConstraintResult::Pass)) {
            sink.add(ReplayFinding::KeepWithoutConstraint, node.keep_seq,
                     "worldline " + std::to_string(item.worldline) +
                         " was kept without a passing constraint result");
        }
    }

    // A KEEP whose worldline never reached the evidence list still
    // needs a verdict while the gate is on.
    if (gate_required) {
        for (const auto& [id, node] : nodes) {
            if (node.keep_seq == 0)
                continue;
            bool known = false;
            for (const ReplayEvidence& item : evidence) {
                if (item.worldline == id) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                sink.add(ReplayFinding::KeepWithoutVerdict, node.keep_seq,
                         "worldline " + std::to_string(id) +
                             " was kept without an admission verdict");
            }
        }
    }

    report.worldlines = nodes.size();
    report.nodes.reserve(nodes.size());
    for (const auto& [id, node] : nodes)
        report.nodes.push_back(node);
    std::sort(report.nodes.begin(), report.nodes.end(),
              [](const ReplayNode& a, const ReplayNode& b) { return a.worldline < b.worldline; });
    return report;
}

} // namespace aura::bus
