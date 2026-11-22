// ──────────────────────────────────────────────────────────────
//  transport.hh — local bus control plane (#5123)
//
//  A Unix socket carries fixed-size control frames; the image travels
//  once over SCM_RIGHTS. Frames carry offsets and lengths, never CCT or
//  patch payloads. The image is sealed shrink/grow, and a peer checks
//  the seals before mapping.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_TRANSPORT_HH
#define AURA_BUS_TRANSPORT_HH

#include "bus/capability.hh"
#include "bus/channel.hh"
#include "bus/constraint.hh"
#include "bus/gate.hh"
#include "bus/image.hh"
#include "bus/replay.hh"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aura::bus {

inline constexpr std::uint32_t kBusAbiMajor = 1;
inline constexpr std::uint32_t kBusAbiMinor = 1;
inline constexpr std::size_t kBusDefaultImageBytes = 256u << 20;
inline constexpr std::size_t kBusMaxControlFrame = 4096;
inline constexpr std::uint32_t kBusMaxLoans = 8;

enum class FrameType : std::uint16_t {
    Hello = 1,
    HelloOk = 2,
    HelloReject = 3,
    Subscribe = 4,
    Unsubscribe = 5,
    Reserve = 6,
    Publish = 7,
    Credit = 8,
    CatchUp = 9,
    Grant = 10,
    Revoke = 11,
    Trial = 12,
    Keep = 13,
    Rollback = 14,
    Query = 15,
    AuditQuery = 16,
    Error = 17,
    Disconnect = 18,
    ConstraintRollback = 19,
    ConstraintInstall = 20,
    // #5379: bind the five-tuple evidence to a trial before KEEP.
    EvidenceBind = 21,
    // #5381: seal an active Canary worldline after its window.
    CanaryConfirm = 22,
    // #5378: append a candidate define source to the string table.
    SourcePublish = 23,
    // #5379: query the coordinator's digest of a trial's patch bytes.
    CandidateDigest = 24,
    // #5377: a session presents a granted token to narrow its own caps
    // to the token's effective (scoped) capability — the judge activates
    // the host's delegated keep+rollback on a single worldline.
    GrantActivate = 25,
    // #5391: bind the judge's held-out evidence to a trial; the
    // coordinator recomputes the admission verdict from it.
    GateBind = 26,
    // #5392: replay the audit log — reconstruct the worldline
    // lineage, check its integrity and reproduce the verdicts.
    AuditReplay = 27,
};

enum class RejectReason : std::uint16_t {
    None = 0,
    Version = 1,
    Schema = 2,
    Seals = 3,
    Permission = 4,
    Workspace = 5,
    ImageSize = 6,
    Protocol = 7,
};

struct FrameHeader {
    std::uint32_t len = 0; // body bytes after this header
    std::uint16_t type = 0;
    std::uint16_t flags = 0;
    std::uint64_t session = 0;
};

struct HelloBody {
    std::uint16_t abi_major = 0;
    std::uint16_t abi_minor = 0;
    std::uint8_t schema_id[16] = {};
    std::uint64_t layout_version = 0;
    std::uint32_t pid = 0;
    std::uint32_t role = 0;
    std::uint64_t caps = 0;
    std::uint64_t channels = 0;
    std::uint64_t max_loans = 0;
    std::uint64_t workspace_id = 0;
    std::uint32_t image_bytes = 0;
    std::uint32_t reserved = 0;
};

struct HelloOkBody {
    std::uint64_t session = 0;
    std::uint64_t caps = 0;
    std::uint64_t image_size = 0;
    std::uint8_t schema_id[16] = {};
    std::uint64_t committed_bytes = 0;
    std::uint64_t channels = 0;
    std::uint64_t max_loans = 0;
};

struct ErrorBody {
    std::uint32_t code = 0;
    std::uint32_t reserved = 0;
    char message[56] = {};
};

struct HelloRejectBody {
    std::uint16_t reason = 0;
    std::uint16_t reserved = 0;
    char detail[60] = {};
};

// Reserve / Publish / Credit body: channel, image offset and length,
// generation and worldline. No payload bytes.
struct GrantBody {
    std::uint64_t parent = 0; // 0 = derive from this session's root grant
    Capability caps{};
};

// Subscribe / CatchUp / Publish reply. `count` is the number of records
// in THIS reply's range (offset..offset+length); `total` is the channel's
// committed record count — the cursor a reader passes back as since-count.
// Added in ABI minor 1 (#5385): before, count carried the total and an
// incremental range decoded to zero rows.
struct ChannelBody {
    std::uint64_t channel = 0;
    std::uint64_t generation = 0;
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint64_t snapshot = 0;
    std::uint64_t total = 0;
};

struct CatchUpBody {
    std::uint64_t channel = 0;
    std::uint64_t generation = 0; // last observed generation
    std::uint64_t count = 0;      // last observed record count
    std::uint64_t reserved = 0;
};

struct AuditQueryBody {
    std::uint64_t worldline = 0; // 0 = all events
    std::uint64_t actor = 0;     // 0 = all tokens
    std::uint64_t since_seq = 0; // 1-based event sequence; 0 = from the start
    std::uint64_t reserved = 0;
};

struct AuditWire {
    std::uint64_t kind = 0;
    std::uint64_t token = 0;
    std::uint64_t worldline = 0;
    std::uint64_t result = 0;
    std::uint64_t version = 0;
    std::uint64_t timestamp_ms = 0;
    std::uint64_t seq = 0; // 1-based position in the coordinator log
    char reason[64] = {};
};

struct AuditReply {
    std::uint64_t count = 0;
    std::uint64_t truncated = 0;
};

// #5392: the audit replay request. The replay always covers the whole
// control-plane log; the fields are reserved for a narrower view.
struct AuditReplayBody {
    std::uint64_t worldline = 0;
    std::uint64_t since_seq = 0;
    std::uint64_t flags = 0;
};

// The audit replay reply: the report, then one row per worldline.
struct AuditReplayReply {
    std::uint64_t events = 0;
    std::uint64_t worldlines = 0;
    std::uint64_t keeps = 0;
    std::uint64_t findings = 0;
    std::uint64_t first_seq = 0;
    std::uint64_t first_kind = 0;
    std::uint64_t truncated = 0;
    std::uint64_t run = 0; // the workspace the log belongs to
    char first_reason[48] = {};
};

// One worldline of the reconstructed lineage.
struct AuditReplayNode {
    std::uint64_t worldline = 0;
    std::uint64_t parent = 0;
    std::uint64_t state = 0;
    std::uint64_t trial_seq = 0;
    std::uint64_t keep_seq = 0;
    std::uint64_t verdict_seq = 0;
    std::uint64_t constraint = 0;
    std::uint64_t reproduced = 0;
    std::uint64_t fingerprint = 0;
    std::uint64_t timestamp_ms = 0;
    std::uint64_t baseline_gen = 0;
    std::uint64_t candidate_hash = 0;
    std::uint64_t workload_sym = 0;
    std::uint64_t seed = 0;
    std::uint64_t verifier_version = 0;
    std::uint64_t gate_verdict = 0;
    std::uint64_t gate_family = 0;
    std::int64_t adjusted_ppm = 0;
    char reason[48] = {};
};

struct GrantReply {
    std::uint64_t token = 0;
};

struct LoanBody {
    std::uint64_t channel = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint64_t generation = 0;
    std::uint64_t worldline = 0;
};

// #5379: candidate evidence five-tuple. Bound to a trial with
// EvidenceBind; KEEP re-checks baseline/candidate against the live
// workspace and the committed patch bytes. Zero fields are "absent".
struct EvidenceBody {
    std::uint64_t worldline = 0;
    std::uint64_t baseline_gen = 0;
    std::uint64_t candidate_hash = 0;
    std::uint64_t workload_sym = 0;
    std::uint64_t seed = 0;
    std::uint64_t verifier_version = 0;
};

// ConstraintInstall body prefix: name/source bytes follow the two lengths.
struct ConstraintWire {
    std::uint32_t name_len = 0;
    std::uint32_t source_len = 0;
};

static_assert(sizeof(FrameHeader) == 16);
static_assert(sizeof(HelloBody) == 80);
static_assert(sizeof(HelloOkBody) == 64);
static_assert(sizeof(ErrorBody) == 64);
static_assert(sizeof(HelloRejectBody) == 64);
static_assert(sizeof(AuditQueryBody) == 32);
static_assert(sizeof(AuditWire) == 120);
static_assert(sizeof(AuditReply) == 16);
static_assert(sizeof(AuditReplayBody) == 24);
static_assert(sizeof(AuditReplayReply) == 112);
static_assert(sizeof(AuditReplayNode) == 192);
static_assert(sizeof(ChannelBody) == 56);
static_assert(sizeof(CatchUpBody) == 32);
static_assert(sizeof(GrantBody) == 56);
static_assert(sizeof(GrantReply) == 8);
static_assert(sizeof(LoanBody) == 40);
static_assert(sizeof(EvidenceBody) == 48);

struct TransportError {
    int code = 0; // errno or RejectReason
    std::string message;
};

// Default socket path: $XDG_RUNTIME_DIR/aura/bus/<workspace-id>.sock.
std::string default_socket_path(std::uint64_t workspace_id);

// memfd_create(MFD_ALLOW_SEALING | MFD_CLOEXEC) + ftruncate + shrink/grow seals.
// Returns -1 on error.
int create_sealed_image(std::size_t bytes, TransportError* error);

// F_GET_SEALS must include SHRINK and GROW.
bool has_required_seals(int fd);

struct Attach {
    int sock = -1;
    int event = -1;
    int image_fd = -1;
    void* base = nullptr;
    std::size_t size = 0;
    std::uint64_t session = 0;
    std::uint64_t caps = 0;

    Attach() = default;
    Attach(const Attach&) = delete;
    Attach& operator=(const Attach&) = delete;
    Attach(Attach&& other) noexcept;
    Attach& operator=(Attach&& other) noexcept;
    ~Attach();

    [[nodiscard]] std::span<const std::byte> view() const {
        return {static_cast<const std::byte*>(base), size};
    }
    void close_all();
};

// Loan accounting per session. reserve() fails at the cap; reclaim()
// returns every outstanding loan on disconnect.
class LoanLedger {
public:
    LoanLedger(std::uint32_t cap = kBusMaxLoans)
        : cap_(cap) {}
    bool reserve();
    void release();
    std::uint64_t reclaim();
    void set_cap(std::uint32_t cap) { cap_ = cap; }
    [[nodiscard]] std::uint32_t outstanding() const { return outstanding_; }
    [[nodiscard]] std::uint64_t reclaimed_total() const { return reclaimed_; }

private:
    std::uint32_t cap_ = kBusMaxLoans;
    std::uint32_t outstanding_ = 0;
    std::uint64_t reclaimed_ = 0;
};

// #5377: one attached peer's state (socket, identity, loans, staging,
// subscriptions, partial-frame buffer). Defined after LoanLedger.
struct PeerSession {
    int conn_fd = -1;
    // The coordinator's reference to the peer's doorbell eventfd. The
    // peer got its own dup at handshake; a commit rings every OTHER
    // session's bell so judges and observers need no polling (#5377).
    int event_fd = -1;
    std::uint64_t session = 0;
    std::uint64_t root_token = 0;
    Capability caps{};
    LoanLedger loans{};
    std::array<LoanLedger, 6> channel_loans{};
    std::uint64_t staging_capacity = 0;
    std::uint64_t staging_used = 0;
    std::uint64_t subscribed = 0; // channel-id bitmask from Subscribe frames
    // Partial-frame accumulation: poll reports readability, never a
    // whole frame. The coordinator drains the socket without blocking.
    std::vector<std::byte> inbuf;
    // Handshake progress: the Hello may arrive in several reads; any
    // ancillary fds before the Hello are unexpected and closed.
    bool hello_done = false;
    std::vector<std::byte> hello_buf;
};

bool send_frame(int fd, FrameType type, std::uint64_t session, std::span<const std::byte> body);
bool recv_frame(int fd, FrameHeader* header, std::span<std::byte> body, TransportError* error);

// Coordinator side: listen, accept peers, handshake, hand out the
// sealed image, and serve every attached session until stop (#5377).
class Server {
public:
    Server() = default;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    ~Server();

    bool listen_at(const std::string& path, std::size_t image_bytes, std::uint64_t workspace_id,
                   TransportError* error);
    // Workspace allow policy and sandbox preset applied at attach. With
    // no policy every role is denied.
    void set_policy(const WorkspacePolicy& policy, SandboxPreset preset = SandboxPreset::Dev) {
        constraints_.set_initial_policy(policy);
        preset_ = preset;
    }
    // build() fills the mapped image before sealing. It must leave a
    // valid header/committed prefix at the committed bytes it reports.
    using BuildFn = std::function<bool(std::span<std::byte>, std::string*)>;
    bool accept_one(const BuildFn& build, Attach* out, TransportError* error);

    // Read until EOF. Control frames are consumed; Reserve consumes a
    // loan, Credit releases one. Returns true when the peer disconnected.
    bool wait_disconnect(TransportError* error);

    // #5377: serve every attached peer concurrently from one coordinator
    // thread (poll over the listener, the wake eventfd and all session
    // sockets). A peer crash/EOF rolls back only that session's pending
    // trials and reclaims only its loans. Returns when stop() is called
    // or the listener dies. The single-peer accept_one/wait_disconnect
    // pair stays for the #5123 harnesses; serve() is the host path.
    void serve(const BuildFn& build);

    // Issue #5128: unblock accept_one / wait_disconnect / serve so a host
    // thread can stop. Shutdown only; the serving thread still closes fds.
    void stop();

    // #5378: the host's live-workspace apply callback. Runs on the
    // coordinator thread AFTER the constraint gate passes and BEFORE the
    // worldline is marked Kept. `patches` are the trial's committed patch
    // records; `sources[i]` is the string-table text for a source-carrier
    // patch (empty for immediate field patches); `generation` is the
    // workspace generation the trial opened at. Return false (with *why)
    // to reject the KEEP; the callback must leave the live workspace
    // byte-identical on failure.
    using ApplyFn = std::function<bool(
        std::uint64_t worldline, std::uint64_t generation, std::span<const PatchRecord> patches,
        std::span<const std::string> sources, bool canary, std::string* why)>;
    void set_apply_callback(ApplyFn fn) { apply_ = std::move(fn); }
    // #5381: restore the live workspace to the pre-canary snapshot.
    // Runs on the coordinator thread when an observation reverts.
    using RevertFn = std::function<bool(std::uint64_t worldline, std::string* why)>;
    void set_revert_callback(RevertFn fn) { revert_ = std::move(fn); }
    // The host advances this after each successful apply; trials record
    // it as their baseline and KEEP rejects a stale one (#5378/#5379).
    void set_workspace_generation(std::uint64_t gen) { workspace_gen_ = gen; }
    [[nodiscard]] std::uint64_t workspace_generation() const { return workspace_gen_; }

    // #5381: canary observation parameters (0 = window/floor disabled).
    void set_canary_window(std::uint64_t ticks) { canary_window_ = ticks; }
    void set_canary_floor(std::uint64_t floor) { canary_floor_ = floor; }
    void set_canary_metric_key(std::uint64_t key) { canary_metric_key_ = key; }
    // #5379: when set, every KEEP must carry a matching evidence
    // five-tuple (the v0.3.1 closed-loop gate). Off by default so the
    // v0.3.0 single-tool surface keeps working.
    void set_evidence_required(bool required) { evidence_required_ = required; }
    // #5391: the statistical admission policy. When required, every
    // KEEP must carry held-out evidence whose recomputed, family
    // corrected statistic clears the bar, and the round must carry a
    // negative control the statistics reject.
    void set_gate_policy(const GatePolicy& policy) {
        gate_policy_ = policy;
        negative_keeps_.configure(policy.budget_window);
    }
    [[nodiscard]] const GatePolicy& gate_policy() const { return gate_policy_; }
    // The evidence bound to a trial (nullptr when unbound).
    [[nodiscard]] const GateEvidenceBody* gate_evidence(std::uint64_t worldline) const;
    // The coordinator's admission verdict for a trial (nullptr when the
    // trial never reached the gate).
    [[nodiscard]] const GateVerdictRecord* gate_verdict(std::uint64_t worldline) const;
    // The negative-KEEP window: how many KEEPs turned out harmful.
    [[nodiscard]] const NegativeKeepWindow& negative_keeps() const { return negative_keeps_; }
    // #5392: replay the audit log — reconstruct the worldline
    // lineage, check its integrity and reproduce the admission
    // verdicts. A read: it changes nothing.
    [[nodiscard]] ReplayReport replay() const;

    // Loan ledgers of the most recent single-peer session (accept_one /
    // wait_disconnect). serve() keeps per-session ledgers in its table.
    [[nodiscard]] const LoanLedger& loans() const { return loans_; }
    // Per-channel loan ledgers: one channel cannot starve another (#5125).
    [[nodiscard]] const LoanLedger& channel_loans(Channel channel) const {
        return channel_loans_[static_cast<std::size_t>(channel)];
    }
    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] const AuditLog& audit() const { return audit_; }
    [[nodiscard]] const WorldlineLedger& worldlines() const { return worldlines_; }
    [[nodiscard]] Capability session_capability() const { return session_caps_; }
    // #5377: live session count on the serve() path (0 while idle).
    [[nodiscard]] std::size_t peer_count() const { return peers_.size(); }
    // Coordinator-side constraint installation (not a wire frame: the
    // constraint object is executable and stays local).
    bool install_constraints(ConstraintSet set, std::string* why);
    // EDSL source binding: register the compiled predicate for a rule name;
    // sessions may install that source under constraint-write. Unknown names
    // are rejected (never silently allow).
    void register_constraint_predicate(std::string name, ConstraintPredicate check);
    // Versioned policy change; requires constraint-write and audits a
    // ConstraintChange, never a plain keep.
    bool install_policy(const Capability& caps, WorkspacePolicy policy, std::string* why);
    [[nodiscard]] const ConstraintStore& constraints() const { return constraints_; }
    // #5379: the trial's bound evidence, if any (nullptr when unbound).
    [[nodiscard]] const EvidenceBody* evidence(std::uint64_t worldline) const;

private:
    // Shared handshake: validate Hello, resolve caps, build/reuse the
    // image, send HelloOk. out != nullptr fills a peer Attach (the
    // accept_one harness path); out == nullptr keeps the mapping on the
    // coordinator only (serve).
    bool handshake(int conn_fd, const HelloBody& hello, const BuildFn& build,
                   std::uint64_t* session_out, Capability* caps_out, std::uint64_t* root_out,
                   int* event_out, std::uint64_t* staging_capacity_out, Attach* out,
                   TransportError* error);
    // Mirror one audit event into the in-process log and the image audit
    // section (when the builder reserved one).
    void record_audit(AuditKind kind, std::uint64_t token, std::uint64_t worldline = 0,
                      std::uint64_t detail = 0);
    void mirror_audit(const AuditEvent& event);
    // Serialize the current constraint set into the image constraints
    // section (header + name/source blob). No-op without a mapping.
    void write_constraints_image();
    // #5377: one control frame from one session. Returns false when the
    // session must end (Disconnect frame); the caller reclaims.
    bool handle_frame(PeerSession& peer, const FrameHeader& header,
                      std::span<const std::byte> body);
    // The shared frame switch, parameterized by the session's state so
    // the single-peer harness path and serve() run the same protocol.
    bool dispatch_frame(int conn_fd, std::uint64_t session, std::uint64_t root_token,
                        Capability& caps, LoanLedger& loans,
                        std::array<LoanLedger, 6>& channel_loans, std::uint64_t staging_capacity,
                        std::uint64_t& staging_used, std::uint64_t* subscribed,
                        const FrameHeader& header, std::span<const std::byte> body);
    // End a session: roll back its pending trials, reclaim its loans,
    // audit Disconnect, close its fds. Never touches other sessions.
    void finish_session(PeerSession& peer);
    // Ring every other session's doorbell after a committed change.
    void notify_peers(std::uint64_t except_session);
    // Append one worldline ledger record into the worldlines channel.
    void project_worldline(std::uint64_t id, WorldlineState state, ConstraintResult cr);
    // #5379: digest of a trial's committed patch bytes (FNV-1a 64).
    std::uint64_t candidate_digest(std::uint64_t worldline) const;
    // #5378: collect the trial's committed patch records from the image.
    std::vector<PatchRecord> trial_patches(std::uint64_t worldline) const;
    // #5381: canary bookkeeping on the coordinator thread.
    void canary_on_keep(std::uint64_t worldline);
    void canary_seal(std::uint64_t worldline, std::uint64_t token);
    void canary_revert(std::uint64_t worldline, std::string_view reason);
    void canary_check_metric(std::uint64_t key, std::uint64_t value);
    // #5391: the statistical admission. `why` names the refusal.
    bool gate_check(std::uint64_t worldline, std::uint64_t token, std::string* why);
    // The family size of a round: the candidates bound to it so far.
    std::uint64_t gate_family_size(std::uint64_t round_id) const;
    // True when the round carries a negative control candidate.
    bool gate_round_has_control(std::uint64_t round_id) const;
    // A KEEP that was reverted (or a control the statistics wrongly
    // accepted) feeds the negative-KEEP budget.
    void note_negative_keep(std::uint64_t worldline);
    // A negative event without a KEEP (a control the statistics
    // wrongly accepted).
    void note_negative_event();
    // Tighten the admission when the negative-KEEP rate is over the
    // budget.
    void gate_tighten_if_needed();
    // Record the round a candidate belongs to; the family grows as
    // more candidates bind.
    void gate_bind_round(std::uint64_t round_id, std::uint64_t worldline);

    std::string path_;
    std::size_t image_bytes_ = kBusDefaultImageBytes;
    std::uint64_t workspace_id_ = 0;
    int listen_fd_ = -1;
    // Single-peer path (accept_one/wait_disconnect) state.
    int conn_fd_ = -1;
    std::uint64_t session_ = 0;
    LoanLedger loans_{};
    std::array<LoanLedger, 6> channel_loans_{};
    std::uint64_t staging_capacity_ = 0;
    std::uint64_t staging_used_ = 0;
    Capability session_caps_{};
    std::uint64_t root_token_ = 0;
    // serve() path: the session table and the wake eventfd stop() writes.
    std::vector<PeerSession> peers_;
    int wake_fd_ = -1;
    SandboxPreset preset_ = SandboxPreset::Dev;
    CapabilityTable caps_table_{};
    AuditLog audit_{};
    std::uint64_t audit_seq_ = 0;
    WorldlineLedger worldlines_{};
    std::uint64_t generation_ = 2;
    ConstraintStore constraints_{};
    std::unordered_map<std::string, ConstraintPredicate> predicate_registry_;
    std::byte* image_base_ = nullptr;
    std::size_t image_size_ = 0;
    // The host's own reference to the shared image fd (#5128): peers get
    // dup'd copies; the mapping and the memfd outlive every session.
    int host_image_fd_ = -1;
    // Trial records already projected into the worldlines channel (#5128).
    std::uint64_t projected_trials_ = 0;
    // #5378: host live-workspace apply + generation.
    ApplyFn apply_;
    std::uint64_t workspace_gen_ = 0;
    RevertFn revert_;
    // #5381: generation the active canary applied at (seal advances to it).
    std::uint64_t canary_applied_gen_ = 0;
    bool evidence_required_ = false;
    // #5379: per-trial evidence bindings.
    std::unordered_map<std::uint64_t, EvidenceBody> evidence_;
    // #5391: the statistical admission state.
    GatePolicy gate_policy_{};
    std::unordered_map<std::uint64_t, GateEvidenceBody> gate_evidence_;
    std::unordered_map<std::uint64_t, GateVerdictRecord> gate_verdicts_;
    // The candidates of each admission round: the family the
    // multiple-comparison correction applies to.
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> gate_rounds_;
    std::vector<std::uint64_t> gate_round_order_;
    // The negative-KEEP window and the slot each KEEP holds in it.
    NegativeKeepWindow negative_keeps_{};
    std::unordered_map<std::uint64_t, std::size_t> gate_keep_slots_;
    // #5381: at most one active canary; its window/floor parameters.
    std::uint64_t canary_worldline_ = 0;
    std::uint64_t canary_window_ = 0;
    std::uint64_t canary_floor_ = 0;
    std::uint64_t canary_metric_key_ = 0;
    std::uint64_t canary_ticks_ = 0;
    std::uint64_t canary_below_ = 0;
};

// Peer side: connect and complete the handshake.
class Client {
public:
    bool connect_to(const std::string& path, std::uint64_t workspace_id, std::uint32_t role,
                    std::uint64_t caps, Attach* out, TransportError* error);
};

} // namespace aura::bus

#endif
