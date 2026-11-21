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
#include "bus/image.hh"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

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
static_assert(sizeof(ChannelBody) == 56);
static_assert(sizeof(CatchUpBody) == 32);
static_assert(sizeof(GrantBody) == 56);
static_assert(sizeof(GrantReply) == 8);
static_assert(sizeof(LoanBody) == 40);

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

bool send_frame(int fd, FrameType type, std::uint64_t session, std::span<const std::byte> body);
bool recv_frame(int fd, FrameHeader* header, std::span<std::byte> body, TransportError* error);

// Coordinator side: listen, accept one peer, handshake, hand out the
// sealed image, and observe disconnect.
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

    // Issue #5128: unblock accept_one / wait_disconnect so a host thread
    // can stop. Shutdown only; the serving thread still closes the fds.
    void stop();

    [[nodiscard]] const LoanLedger& loans() const { return loans_; }
    // Per-channel loan ledgers: one channel cannot starve another (#5125).
    [[nodiscard]] const LoanLedger& channel_loans(Channel channel) const {
        return channel_loans_[static_cast<std::size_t>(channel)];
    }
    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] const AuditLog& audit() const { return audit_; }
    [[nodiscard]] const WorldlineLedger& worldlines() const { return worldlines_; }
    [[nodiscard]] Capability session_capability() const { return session_caps_; }
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

private:
    // Mirror one audit event into the in-process log and the image audit
    // section (when the builder reserved one).
    void record_audit(AuditKind kind, std::uint64_t token, std::uint64_t worldline = 0,
                      std::uint64_t detail = 0);
    void mirror_audit(const AuditEvent& event);
    // Serialize the current constraint set into the image constraints
    // section (header + name/source blob). No-op without a mapping.
    void write_constraints_image();

    std::string path_;
    std::size_t image_bytes_ = kBusDefaultImageBytes;
    std::uint64_t workspace_id_ = 0;
    int listen_fd_ = -1;
    int conn_fd_ = -1;
    std::uint64_t session_ = 0;
    LoanLedger loans_{};
    std::array<LoanLedger, 6> channel_loans_{};
    std::uint64_t staging_capacity_ = 0;
    std::uint64_t staging_used_ = 0;
    SandboxPreset preset_ = SandboxPreset::Dev;
    CapabilityTable caps_table_{};
    AuditLog audit_{};
    std::uint64_t audit_seq_ = 0;
    WorldlineLedger worldlines_{};
    Capability session_caps_{};
    std::uint64_t root_token_ = 0;
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
};

// Peer side: connect and complete the handshake.
class Client {
public:
    bool connect_to(const std::string& path, std::uint64_t workspace_id, std::uint32_t role,
                    std::uint64_t caps, Attach* out, TransportError* error);
};

} // namespace aura::bus

#endif
