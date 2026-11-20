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

#include "bus/image.hh"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace aura::bus {

inline constexpr std::uint32_t kBusAbiMajor = 1;
inline constexpr std::uint32_t kBusAbiMinor = 0;
inline constexpr std::size_t kBusDefaultImageBytes = 256u << 20;
inline constexpr std::size_t kBusMaxControlFrame = 4096;
inline constexpr std::uint32_t kBusMaxLoans = 8;
inline constexpr std::uint64_t kCapRead = 1ull << 0;
inline constexpr std::uint64_t kCapPublish = 1ull << 1;
inline constexpr std::uint64_t kCapTrial = 1ull << 2;
inline constexpr std::uint64_t kCapKeep = 1ull << 3;
inline constexpr std::uint64_t kCapAudit = 1ull << 4;
inline constexpr std::uint64_t kCapAll = kCapRead | kCapPublish | kCapTrial | kCapKeep | kCapAudit;

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
};

struct HelloRejectBody {
    std::uint16_t reason = 0;
    std::uint16_t reserved = 0;
    char detail[60] = {};
};

// Reserve / Publish / Credit body: channel, image offset and length,
// generation and worldline. No payload bytes.
struct LoanBody {
    std::uint64_t channel = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint64_t generation = 0;
    std::uint64_t worldline = 0;
};

static_assert(sizeof(FrameHeader) == 16);
static_assert(sizeof(HelloBody) == 64);
static_assert(sizeof(HelloOkBody) == 48);
static_assert(sizeof(HelloRejectBody) == 64);
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
    explicit LoanLedger(std::uint32_t cap = kBusMaxLoans)
        : cap_(cap) {}
    bool reserve();
    void release();
    std::uint64_t reclaim();
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
    // build() fills the mapped image before sealing. It must leave a
    // valid header/committed prefix at the committed bytes it reports.
    using BuildFn = std::function<bool(std::span<std::byte>, std::string*)>;
    bool accept_one(const BuildFn& build, Attach* out, TransportError* error);

    // Read until EOF. Control frames are consumed; Reserve consumes a
    // loan, Credit releases one. Returns true when the peer disconnected.
    bool wait_disconnect(TransportError* error);

    [[nodiscard]] const LoanLedger& loans() const { return loans_; }
    [[nodiscard]] const std::string& path() const { return path_; }

private:
    std::string path_;
    std::size_t image_bytes_ = kBusDefaultImageBytes;
    std::uint64_t workspace_id_ = 0;
    int listen_fd_ = -1;
    int conn_fd_ = -1;
    std::uint64_t session_ = 0;
    LoanLedger loans_{};
    std::uint64_t staging_capacity_ = 0;
    std::uint64_t staging_used_ = 0;
};

// Peer side: connect and complete the handshake.
class Client {
public:
    bool connect_to(const std::string& path, std::uint64_t workspace_id, std::uint32_t role,
                    std::uint64_t caps, Attach* out, TransportError* error);
};

} // namespace aura::bus

#endif
