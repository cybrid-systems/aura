// ──────────────────────────────────────────────────────────────
//  capability.hh — session capabilities, grants and worldline
//  ownership for the local bus (#5124).
//
//  Capabilities are explicit bits plus a channel mask and optional
//  narrowing (workspace, worldline, expiry, loan quota). They can only
//  shrink; a grant tree lets a coordinator revoke an ancestor and every
//  descendant with it. No environment default and no role trust.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_CAPABILITY_HH
#define AURA_BUS_CAPABILITY_HH

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aura::bus {

enum CapBit : std::uint64_t {
    kCapRead = 1ull << 0,
    kCapPublish = 1ull << 1,
    kCapTrial = 1ull << 2,
    kCapKeep = 1ull << 3,
    kCapRollback = 1ull << 4,
    kCapGrant = 1ull << 5,
    kCapConstraintWrite = 1ull << 6,
};

inline constexpr std::uint64_t kCapAll =
    kCapRead | kCapPublish | kCapTrial | kCapKeep | kCapRollback | kCapGrant | kCapConstraintWrite;

const char* capability_name(std::uint64_t bit);

struct Capability {
    std::uint64_t bits = 0;
    std::uint64_t channels = 0; // channel-id bitmask, 0..63
    std::uint64_t workspace = 0;
    std::uint64_t max_loans = 0;   // 0 = no derived quota
    std::uint64_t expires_gen = 0; // 0 = no expiry
    std::uint64_t worldline = 0;   // 0 = unscoped

    friend constexpr bool operator==(const Capability&, const Capability&) = default;
};

// True when child is a true subset of parent: every bit, channel,
// quota and scope is no wider, and the workspace is identical.
bool attenuates(const Capability& parent, const Capability& child, std::string* why);

Capability intersect(const Capability& a, const Capability& b);

// Check one operation against a capability. On failure *missing names
// the missing bit (or the failing scope) for the error frame.
bool allows(const Capability& caps, std::uint64_t bit, std::uint64_t channel,
            std::uint64_t generation, std::uint64_t worldline, std::string* why);

struct GrantRecord {
    std::uint64_t token = 0;
    std::uint64_t parent = 0; // 0 = root issued by the coordinator
    Capability caps{};
    bool revoked = false;
};

// Coordinator-issued tokens and the capability derivation tree.
class CapabilityTable {
public:
    CapabilityTable();

    // Root grant at attach. Returns an unguessable non-zero token.
    std::uint64_t issue_root(const Capability& caps);
    // Derive a child. Fails when the parent is missing/revoked/lacks
    // grant, the child is not a subset, or the workspace differs.
    std::optional<std::uint64_t> derive(std::uint64_t parent, const Capability& child,
                                        std::string* why);
    // Revoke token and every descendant; returns the number revoked.
    std::size_t revoke(std::uint64_t token);
    bool effective(std::uint64_t token, Capability* out) const;
    bool revoked(std::uint64_t token) const;
    std::size_t record_count() const { return records_.size(); }

private:
    std::vector<GrantRecord> records_;
    std::uint64_t next_token_ = 1;
    std::uint64_t secret_ = 0;
};

struct TrialRecord {
    std::uint64_t id = 0;
    std::uint64_t owner = 0; // grant token that opened the trial
    std::uint64_t workspace = 0;
    // #5378: host workspace generation when the trial opened; KEEP
    // rejects a stale baseline (another worldline applied first).
    std::uint64_t base_generation = 0;
    bool pending = false;
    bool kept = false;
    bool rolled_back = false;
    bool loan_outstanding = false;
    // #5378: the KEEP applied a source-carrier patch to the live AST
    // (vs an immediate-only image-evidence trial).
    bool applied_live = false;
    // #5381: observation mode — kept=true but the live apply is under a
    // canary window; seal confirms Kept, a revert rolls back to Rolled.
    bool canary = false;
};

// Trial worldlines and their ownership. keep/rollback require either
// ownership or a capability explicitly scoped to that worldline.
class WorldlineLedger {
public:
    std::uint64_t open_trial(std::uint64_t owner, std::uint64_t workspace,
                             std::uint64_t base_generation = 0);
    bool keep(std::uint64_t id, std::uint64_t owner, const Capability& caps, std::string* why,
              bool applied_live = false);
    bool rollback(std::uint64_t id, std::uint64_t owner, const Capability& caps, std::string* why);
    void release_loan(std::uint64_t id);
    const TrialRecord* find(std::uint64_t id) const;
    // #5395: the trials that are still pending (the soak lane watches
    // this number for a residue).
    std::size_t pending_count() const;
    std::vector<std::uint64_t> end_session(std::uint64_t owner);

    // #5381: canary transitions. enter_canary requires a kept, applied
    // trial; seal_canary ends observation as Kept; revert_canary rolls
    // the observation back (the caller restores the workspace snapshot).
    bool enter_canary(std::uint64_t id, std::string* why);
    bool seal_canary(std::uint64_t id, std::string* why);
    bool revert_canary(std::uint64_t id, std::string* why);
    // #5381: rolling back an already-kept worldline (a sealed canary the
    // judge reverts). Ownership/scope rules are the same as rollback.
    bool rollback_kept(std::uint64_t id, std::uint64_t owner, const Capability& caps,
                       std::string* why);

private:
    std::vector<TrialRecord> records_;
    std::uint64_t next_id_ = 1;
};

enum class AuditKind : std::uint16_t {
    Attach = 1,
    Grant = 2,
    Revoke = 3,
    Trial = 4,
    Keep = 5,
    Rollback = 6,
    Disconnect = 7,
    Denied = 8,
    Publish = 9,
    Subscribe = 10,
    ConstraintResult = 11,
    ConstraintChange = 12,
    ConstraintRollback = 13,
    // #5381: canary observation lifecycle events.
    CanaryStart = 14,
    CanarySeal = 15,
    CanaryRevert = 16,
    // #5391: statistical admission events. GateBind records the
    // judge's held-out evidence, GateVerdict the coordinator's own
    // recomputed decision, GateTighten a tightened admission policy.
    GateBind = 17,
    GateVerdict = 18,
    GateTighten = 19,
    // #5393: the admission round state machine.
    RoundOpen = 20,   // a round was opened
    RoundTrial = 21,  // the round's candidate was opened
    RoundJudge = 22,  // the candidate went to the judge
    RoundSeal = 23,   // the round sealed (the worldline is kept)
    RoundRoll = 24,   // the round rolled back (fail closed)
    RoundBudget = 25, // the round was closed by the budget
    RoundResume = 26, // the host resumed from its checkpoint
    // #5394: the degradation detector.
    DegradeAlert = 27,  // the detector saw a sustained degradation
    DegradeConfig = 28, // the detector's configuration changed
};

struct AuditEvent {
    AuditKind kind = AuditKind::Attach;
    std::uint64_t token = 0;
    std::uint64_t worldline = 0;
    std::uint64_t detail = 0;
    std::uint64_t result = 0;
    std::uint64_t constraint_version = 0;
    std::uint64_t timestamp_ms = 0;
    std::string reason;
};

class AuditLog {
public:
    // Every event is also handed to this sink (the coordinator mirrors the
    // log into the image audit section; #5122/#5126).
    using Sink = std::function<void(const AuditEvent&)>;
    void set_sink(Sink sink) { sink_ = std::move(sink); }

    void add(AuditKind kind, std::uint64_t token, std::uint64_t worldline = 0,
             std::uint64_t detail = 0);
    void add_constraint(AuditKind kind, std::uint64_t token, std::uint64_t worldline,
                        std::uint64_t result, std::uint64_t version, std::string reason);
    const std::vector<AuditEvent>& events() const { return events_; }
    std::size_t count(AuditKind kind) const;
    std::vector<AuditEvent> for_worldline(std::uint64_t worldline) const;

private:
    std::vector<AuditEvent> events_;
    Sink sink_;
};

// Workspace allow policy. A role is only a hint; the coordinator grants
// the subset of the request the policy allows for that role.
class WorkspacePolicy {
public:
    void allow(std::uint32_t role, const Capability& max);
    Capability resolve(std::uint32_t role, const Capability& requested, std::uint64_t workspace,
                       bool* denied) const;
    [[nodiscard]] bool empty() const { return entries_.empty(); }

private:
    struct Entry {
        std::uint32_t role = 0;
        Capability max{};
    };
    std::vector<Entry> entries_;
};

// Sandbox presets clamp the workspace policy. Unknown roles and unset
// presets never widen a capability.
enum class SandboxPreset : std::uint8_t { Dev = 0, Ci = 1, Prod = 2 };

Capability clamp_preset(const Capability& caps, SandboxPreset preset);

} // namespace aura::bus

#endif
