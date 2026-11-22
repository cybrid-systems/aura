// ──────────────────────────────────────────────────────────────
//  transport.cpp — Unix-socket control plane + sealed memfd (#5123)
//
//  Frames are fixed-size headers plus small bodies. The image fd and the
//  doorbell fd travel once over SCM_RIGHTS; later frames carry only
//  channel/offset/length/generation/worldline.
// ──────────────────────────────────────────────────────────────
#include "bus/transport.hh"

#include "bus/channel.hh"
#include "bus/image_schema.hh"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <deque>
#include <vector>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

namespace aura::bus {
namespace {

    constexpr std::uint64_t kSealMask = F_SEAL_SHRINK | F_SEAL_GROW;

    std::atomic<std::uint64_t> g_session_counter{0};

    int memfd_create_cloexec(const char* name, unsigned flags) {
        return static_cast<int>(syscall(SYS_memfd_create, name, flags));
    }

    bool read_all(int fd, void* buf, std::size_t len) {
        auto* p = static_cast<std::byte*>(buf);
        while (len > 0) {
            const ssize_t n = ::read(fd, p, len);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (n == 0)
                return false;
            p += n;
            len -= static_cast<std::size_t>(n);
        }
        return true;
    }

    bool write_all(int fd, const void* buf, std::size_t len) {
        const auto* p = static_cast<const std::byte*>(buf);
        while (len > 0) {
            const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (n == 0)
                return false;
            p += n;
            len -= static_cast<std::size_t>(n);
        }
        return true;
    }

    bool send_with_fds(int fd, const void* data, std::size_t len, const int* fds,
                       std::size_t nfds) {
        struct iovec iov{};
        iov.iov_base = const_cast<void*>(data);
        iov.iov_len = len;
        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        std::byte cmsgbuf[CMSG_SPACE(sizeof(int) * 4)] = {};
        if (nfds > 0) {
            msg.msg_control = cmsgbuf;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
            auto* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
            std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfds);
        }
        while (true) {
            const ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            // A control message is attached to the whole first byte; a short
            // write still delivered the fds, so only the remainder is retried
            // without them.
            if (static_cast<std::size_t>(n) == len)
                return true;
            auto* rest = static_cast<const std::byte*>(data) + n;
            const std::size_t left = len - static_cast<std::size_t>(n);
            msg.msg_control = nullptr;
            msg.msg_controllen = 0;
            iov.iov_base = const_cast<void*>(static_cast<const void*>(rest));
            iov.iov_len = left;
            return write_all(fd, rest, left);
        }
    }

    // One recvmsg that may carry SCM_RIGHTS fds. Returns the byte count
    // (0 = EOF, -1 = error/EAGAIN); received fds are appended to fds_out.
    ssize_t recv_msg(int fd, void* buf, std::size_t len, std::vector<int>* fds_out) {
        struct iovec iov{};
        iov.iov_base = buf;
        iov.iov_len = len;
        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        std::byte cmsgbuf[CMSG_SPACE(sizeof(int) * 4)] = {};
        msg.msg_control = cmsgbuf;
        msg.msg_controllen = sizeof(cmsgbuf);
        ssize_t n = 0;
        do {
            n = ::recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
        } while (n < 0 && errno == EINTR);
        if (n <= 0)
            return n;
        for (auto* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
                continue;
            const std::size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (std::size_t i = 0; i < count; ++i) {
                int received = -1;
                std::memcpy(&received, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
                fds_out->push_back(received);
            }
        }
        return n;
    }

    bool recv_header(int fd, FrameHeader* header, int* fds, std::size_t max_fds,
                     std::size_t* nfds) {
        std::vector<int> got;
        const ssize_t n = recv_msg(fd, header, sizeof(FrameHeader), &got);
        if (n <= 0)
            return false;
        *nfds = 0;
        for (const int f : got) {
            if (*nfds < max_fds)
                fds[(*nfds)++] = f;
            else
                ::close(f);
        }
        if (static_cast<std::size_t>(n) != sizeof(FrameHeader))
            return false;
        return true;
    }

    bool channel_from(std::uint64_t id, Channel* out) {
        if (id < 1 || id > 5)
            return false;
        *out = static_cast<Channel>(id);
        return true;
    }

    bool set_reject(int fd, std::uint64_t session, RejectReason reason, std::string_view detail) {
        HelloRejectBody body{};
        body.reason = static_cast<std::uint16_t>(reason);
        const std::size_t n = std::min(detail.size(), sizeof(body.detail) - 1);
        std::memcpy(body.detail, detail.data(), n);
        return send_frame(
            fd, FrameType::HelloReject, session,
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(&body), sizeof(body)));
    }

} // namespace

std::string default_socket_path(std::uint64_t workspace_id) {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    std::string dir = xdg && *xdg ? std::string(xdg) + "/aura/bus"
                                  : "/tmp/aura-" + std::to_string(::getuid()) + "/bus";
    return dir + "/" + std::to_string(workspace_id) + ".sock";
}

int create_sealed_image(std::size_t bytes, TransportError* error) {
    const int fd = memfd_create_cloexec("aura-bus-image", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (fd < 0) {
        if (error)
            *error = {errno, "memfd_create failed"};
        return -1;
    }
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        if (error)
            *error = {errno, "ftruncate failed"};
        ::close(fd);
        return -1;
    }
    if (fcntl(fd, F_ADD_SEALS, kSealMask) != 0) {
        if (error)
            *error = {errno, "F_ADD_SEALS failed"};
        ::close(fd);
        return -1;
    }
    return fd;
}

bool has_required_seals(int fd) {
    const int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0)
        return false;
    return (static_cast<unsigned>(seals) & static_cast<unsigned>(kSealMask)) ==
           static_cast<unsigned>(kSealMask);
}

Attach::Attach(Attach&& other) noexcept
    : sock(other.sock)
    , event(other.event)
    , image_fd(other.image_fd)
    , base(other.base)
    , size(other.size)
    , session(other.session)
    , caps(other.caps) {
    other.sock = -1;
    other.event = -1;
    other.image_fd = -1;
    other.base = nullptr;
    other.size = 0;
}

Attach& Attach::operator=(Attach&& other) noexcept {
    if (this != &other) {
        close_all();
        sock = other.sock;
        event = other.event;
        image_fd = other.image_fd;
        base = other.base;
        size = other.size;
        session = other.session;
        caps = other.caps;
        other.sock = -1;
        other.event = -1;
        other.image_fd = -1;
        other.base = nullptr;
        other.size = 0;
    }
    return *this;
}

Attach::~Attach() {
    close_all();
}

void Attach::close_all() {
    if (base && size)
        ::munmap(base, size);
    base = nullptr;
    size = 0;
    if (sock >= 0)
        ::close(sock);
    sock = -1;
    if (event >= 0)
        ::close(event);
    event = -1;
    if (image_fd >= 0)
        ::close(image_fd);
    image_fd = -1;
}

bool LoanLedger::reserve() {
    if (outstanding_ >= cap_)
        return false;
    ++outstanding_;
    return true;
}

void LoanLedger::release() {
    if (outstanding_ > 0)
        --outstanding_;
}

std::uint64_t LoanLedger::reclaim() {
    reclaimed_ += outstanding_;
    outstanding_ = 0;
    return reclaimed_;
}

bool send_frame(int fd, FrameType type, std::uint64_t session, std::span<const std::byte> body) {
    FrameHeader header{};
    header.len = static_cast<std::uint32_t>(body.size());
    header.type = static_cast<std::uint16_t>(type);
    header.session = session;
    std::vector<std::byte> out(sizeof(header) + body.size());
    std::memcpy(out.data(), &header, sizeof(header));
    if (!body.empty())
        std::memcpy(out.data() + sizeof(header), body.data(), body.size());
    return write_all(fd, out.data(), out.size());
}

bool recv_frame(int fd, FrameHeader* header, std::span<std::byte> body, TransportError* error) {
    std::size_t nfds = 0;
    int fds[4] = {-1, -1, -1, -1};
    if (!recv_header(fd, header, fds, 4, &nfds)) {
        if (error)
            *error = {errno ? errno : EPIPE, "peer disconnected"};
        return false;
    }
    for (std::size_t i = 0; i < nfds; ++i)
        ::close(fds[i]); // unexpected ancillary data on a control frame
    if (header->len > kBusMaxControlFrame || header->len > body.size()) {
        if (error)
            *error = {EMSGSIZE, "control frame body too large"};
        return false;
    }
    if (header->len && !read_all(fd, body.data(), header->len)) {
        if (error)
            *error = {errno ? errno : EPIPE, "short control frame"};
        return false;
    }
    return true;
}

Server::~Server() {
    for (auto& peer : peers_) {
        if (peer.conn_fd >= 0)
            ::close(peer.conn_fd);
        if (peer.event_fd >= 0)
            ::close(peer.event_fd);
    }
    if (conn_fd_ >= 0)
        ::close(conn_fd_);
    if (listen_fd_ >= 0)
        ::close(listen_fd_);
    if (wake_fd_ >= 0)
        ::close(wake_fd_);
    if (!path_.empty())
        ::unlink(path_.c_str());
    // The shared image lives as long as the host (#5128); unhost releases
    // the mapping and the host's fd reference. Peers already detached keep
    // their own dup'd fds until they close them.
    if (image_base_ != nullptr)
        ::munmap(image_base_, image_size_);
    if (host_image_fd_ >= 0)
        ::close(host_image_fd_);
}

void Server::stop() {
    // The flag comes first: the serving thread leaves on the flag, not
    // on the bell (a stray write must not stop the coordinator).
    stop_requested_ = true;
    // Shutdown, not close: the serving thread may be blocked in accept
    // or recv on these fds and must observe the error before closing.
    if (listen_fd_ >= 0)
        ::shutdown(listen_fd_, SHUT_RDWR);
    if (conn_fd_ >= 0)
        ::shutdown(conn_fd_, SHUT_RDWR);
    // #5377: wake the poll loop; it observes the counter and exits.
    if (wake_fd_ >= 0) {
        const std::uint64_t one = 1;
        ssize_t ignored = ::write(wake_fd_, &one, sizeof(one));
        (void)ignored;
    }
}

bool Server::listen_at(const std::string& path, std::size_t image_bytes, std::uint64_t workspace_id,
                       TransportError* error) {
    audit_.set_sink([this](const AuditEvent& event) { mirror_audit(event); });
    path_ = path;
    image_bytes_ = image_bytes;
    workspace_id_ = workspace_id;
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        if (error)
            *error = {ENAMETOOLONG, "socket path too long"};
        return false;
    }
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        for (std::size_t at = 1; at <= dir.size(); ++at) {
            if (at == dir.size() || dir[at] == '/') {
                const std::string part = dir.substr(0, at);
                if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) {
                    if (error)
                        *error = {errno, "mkdir failed"};
                    return false;
                }
            }
        }
    }
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        if (error)
            *error = {errno, "socket failed"};
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error)
            *error = {errno, "bind failed"};
        return false;
    }
    if (::chmod(path.c_str(), 0600) != 0) {
        if (error)
            *error = {errno, "chmod failed"};
        return false;
    }
    // #5377: several tools attach at once; the backlog holds connections
    // waiting for the coordinator's accept pass.
    if (::listen(listen_fd_, 16) != 0) {
        if (error)
            *error = {errno, "listen failed"};
        return false;
    }
    return true;
}

// ── Handshake (#5123, shared by accept_one and serve) ────────
// Validates the Hello, resolves capabilities, creates or reuses the
// persistent image and sends HelloOk with the image + doorbell fds.
// On success the session fields are filled and the coordinator keeps
// conn_fd; on failure the connection is closed by the caller's guard.
bool Server::handshake(int conn_fd, const HelloBody& hello, const BuildFn& build,
                       std::uint64_t* session_out, Capability* caps_out, std::uint64_t* root_out,
                       int* event_out, std::uint64_t* staging_capacity_out, Attach* out,
                       TransportError* error) {
    // #5395: the handshake's CPU cost, measured on the coordinator
    // thread.
    const std::uint64_t handshake_started_ns = slo_thread_cpu_ns();
    ucred cred{};
    socklen_t cred_len = sizeof(cred);
    if (getsockopt(conn_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 ||
        cred.uid != ::getuid()) {
        set_reject(conn_fd, 0, RejectReason::Permission, "peer uid differs");
        if (error)
            *error = {EPERM, "peer uid differs"};
        return false;
    }
    const SchemaId header_id = header_schema_id();
    std::uint8_t expected[16] = {};
    std::memcpy(expected, &header_id.a, 8);
    std::memcpy(expected + 8, &header_id.b, 8);
    const auto reject = [&](RejectReason reason, std::string_view why, int code) {
        set_reject(conn_fd, 0, reason, why);
        if (error)
            *error = {code, std::string(why)};
        return false;
    };
    if (hello.abi_major != kBusAbiMajor)
        return reject(RejectReason::Version, "ABI major mismatch", EPROTO);
    if (hello.abi_minor < 1)
        return reject(RejectReason::Version, "peer predates ChannelBody.total (#5385)", EPROTO);
    if (std::memcmp(hello.schema_id, expected, sizeof(expected)) != 0)
        return reject(RejectReason::Schema, "schema id mismatch", EPROTO);
    if (hello.workspace_id != workspace_id_)
        return reject(RejectReason::Workspace, "workspace id mismatch", EPROTO);

    Capability requested{};
    requested.bits = hello.caps;
    requested.channels = hello.channels;
    requested.max_loans = hello.max_loans;
    requested.workspace = workspace_id_;
    bool policy_denied = false;
    Capability granted =
        constraints_.policy().resolve(hello.role, requested, workspace_id_, &policy_denied);
    granted = clamp_preset(granted, preset_);
    if (hello.caps != 0 && granted.bits == 0) {
        record_audit(AuditKind::Denied, 0, 0, hello.role);
        return reject(RejectReason::Permission, "no capability allowed for this role", EACCES);
    }
    const std::uint64_t root_token = caps_table_.issue_root(granted);

    // Issue #5128: one bus = one workspace = one live image. The image
    // outlives peer sessions: the first attach creates and builds it,
    // every later peer is handed the SAME memfd, so the next tool reads
    // the previous tool's committed records instead of a fresh layout.
    // Disconnect withdraws trials and loans, never committed evidence.
    std::size_t bytes = image_size_;
    int image_fd = -1;
    void* base = nullptr;
    const bool first_image = image_base_ == nullptr;
    if (first_image) {
        bytes = hello.image_bytes == 0 ? image_bytes_
                                       : std::min<std::size_t>(hello.image_bytes, image_bytes_);
        if (bytes < kImageHeaderSize)
            return reject(RejectReason::ImageSize, "image reservation too small", EINVAL);
        image_fd = create_sealed_image(bytes, error);
        if (image_fd < 0)
            return false;
        base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, image_fd, 0);
        if (base == MAP_FAILED) {
            ::close(image_fd);
            if (error)
                *error = {errno, "mmap failed"};
            return false;
        }
        std::memset(base, 0, bytes);
        std::string why;
        if (!build({static_cast<std::byte*>(base), bytes}, &why)) {
            ::munmap(base, bytes);
            ::close(image_fd);
            if (error)
                *error = {EINVAL, "image build failed: " + why};
            return false;
        }
    } else if (hello.image_bytes != 0 && hello.image_bytes < image_size_) {
        // The sealed image cannot shrink; a smaller reservation is a hard
        // error, never a silent resize.
        return reject(RejectReason::ImageSize, "image already hosted at a larger size", EINVAL);
    } else {
        // The host keeps its own fd reference and mapping; the peer gets
        // dup'd copies, so its detach cannot take the image down.
        image_fd = ::dup(host_image_fd_);
        if (image_fd < 0) {
            if (error)
                *error = {errno, "dup host image failed"};
            return false;
        }
        base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, image_fd, 0);
        if (base == MAP_FAILED) {
            ::close(image_fd);
            if (error)
                *error = {errno, "mmap host image failed"};
            return false;
        }
    }
    const ImageCheck check = validate_image({static_cast<const std::byte*>(base), bytes});
    if (!check.ok()) {
        ::munmap(base, bytes);
        ::close(image_fd);
        if (error)
            *error = {EINVAL, std::string("image validation failed: ") + check.detail};
        return false;
    }
    const ImageHeader ih = load_header({static_cast<const std::byte*>(base), bytes});
    generation_ = ih.committed_gen;

    const int event_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event_fd < 0) {
        ::munmap(base, bytes);
        ::close(image_fd);
        if (error)
            *error = {errno, "eventfd failed"};
        return false;
    }

    const std::uint64_t session =
        (static_cast<std::uint64_t>(::getpid()) << 32) ^ ++g_session_counter;
    HelloOkBody ok{};
    ok.session = session;
    ok.caps = granted.bits;
    ok.channels = granted.channels;
    ok.max_loans = granted.max_loans;
    ok.image_size = bytes;
    std::memcpy(ok.schema_id, expected, sizeof(expected));
    ok.committed_bytes = ih.committed_bytes;
    FrameHeader ok_header{};
    ok_header.len = sizeof(ok);
    ok_header.type = static_cast<std::uint16_t>(FrameType::HelloOk);
    ok_header.session = session;
    std::vector<std::byte> packet(sizeof(ok_header) + sizeof(ok));
    std::memcpy(packet.data(), &ok_header, sizeof(ok_header));
    std::memcpy(packet.data() + sizeof(ok_header), &ok, sizeof(ok));
    const int fds[2] = {image_fd, event_fd};
    if (!send_with_fds(conn_fd, packet.data(), packet.size(), fds, 2)) {
        ::munmap(base, bytes);
        ::close(image_fd);
        ::close(event_fd);
        if (error)
            *error = {errno, "sendmsg HelloOk failed"};
        return false;
    }

    // Reserve grants slices of a fixed staging window; exhaustion is a
    // hard error, never a resize.
    const std::uint64_t staging_capacity = std::min<std::uint64_t>(
        64u << 10, ih.committed_bytes < bytes ? bytes - ih.committed_bytes : 0);
    if (first_image) {
        image_base_ = static_cast<std::byte*>(base);
        image_size_ = bytes;
        host_image_fd_ = ::dup(image_fd);
        if (out != nullptr) {
            // The host's mapping must outlive the peer's Attach: give the
            // peer its own mapping of the same memfd.
            void* peer_base =
                ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, image_fd, 0);
            if (peer_base == MAP_FAILED) {
                ::munmap(base, bytes);
                image_base_ = nullptr;
                ::close(host_image_fd_);
                host_image_fd_ = -1;
                ::close(image_fd);
                ::close(event_fd);
                if (error)
                    *error = {errno, "mmap peer image failed"};
                return false;
            }
            base = peer_base;
        }
        // serve(): the coordinator keeps this mapping (image_base_); the
        // peer maps its own dup'd fd. Nothing to unmap at session end.
    } else if (out == nullptr) {
        ::munmap(base, bytes);
        base = nullptr;
    }
    write_constraints_image();
    record_audit(AuditKind::Attach, root_token, 0, hello.pid);
    // #5395: the handshake's CPU cost is part of the bus's overhead.
    ++slo_.attaches;
    slo_.attach_cpu_ns += slo_thread_cpu_ns() - handshake_started_ns;
    *session_out = session;
    *caps_out = granted;
    *root_out = root_token;
    *event_out = event_fd;
    *staging_capacity_out = staging_capacity;
    if (out != nullptr) {
        // The harness Attach owns the doorbell fd it was handed; the
        // single-peer server keeps no reference of its own.
        out->event = event_fd;
        out->image_fd = image_fd;
        out->base = base;
        out->size = bytes;
        out->session = session;
        out->caps = ok.caps;
    } else {
        ::close(image_fd);
    }
    return true;
}

bool Server::accept_one(const BuildFn& build, Attach* out, TransportError* error) {
    if (listen_fd_ < 0) {
        if (error)
            *error = {EBADF, "server is not listening"};
        return false;
    }
    conn_fd_ = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (conn_fd_ < 0) {
        if (error)
            *error = {errno, "accept failed"};
        return false;
    }
    // Issue #5383: a rejected attach must close the connection; the serve
    // loop keeps accepting. The guard releases on EVERY early return below
    // (HelloReject sent or not); the success path disarms it because the
    // server keeps conn_fd_ to observe the disconnect.
    struct RejectClose {
        int* fd;
        bool armed = true;
        ~RejectClose() {
            if (armed && *fd >= 0) {
                ::close(*fd);
                *fd = -1;
            }
        }
    } reject_close{&conn_fd_};

    FrameHeader header{};
    std::vector<std::byte> body_buf(kBusMaxControlFrame);
    if (!recv_frame(conn_fd_, &header, body_buf, error))
        return false;
    if (header.type != static_cast<std::uint16_t>(FrameType::Hello) ||
        header.len != sizeof(HelloBody)) {
        set_reject(conn_fd_, 0, RejectReason::Protocol, "expected Hello");
        if (error)
            *error = {EPROTO, "expected Hello"};
        return false;
    }
    HelloBody hello{};
    std::memcpy(&hello, body_buf.data(), sizeof(hello));
    int event_fd = -1;
    if (!handshake(conn_fd_, hello, build, &session_, &session_caps_, &root_token_, &event_fd,
                   &staging_capacity_, out, error))
        return false;
    if (session_caps_.max_loans > 0) {
        const auto cap = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(session_caps_.max_loans, kBusMaxLoans));
        for (auto& ledger : channel_loans_)
            ledger.set_cap(cap);
    }
    staging_used_ = 0;
    // The server keeps conn_fd_ to observe disconnect; Attach::sock stays -1.
    reject_close.armed = false;
    return true;
}

void Server::record_audit(AuditKind kind, std::uint64_t token, std::uint64_t worldline,
                          std::uint64_t detail) {
    audit_.add(kind, token, worldline, detail);
}

void Server::mirror_audit(const AuditEvent& event) {
    if (image_base_ == nullptr)
        return;
    AuditRecord rec{};
    rec.seq = ++audit_seq_;
    rec.kind = static_cast<std::uint16_t>(event.kind);
    rec.actor = static_cast<std::uint32_t>(event.token);
    rec.target = event.worldline;
    rec.value = event.detail != 0 ? event.detail : event.result;
    std::string why;
    if (!audit_append({image_base_, image_size_}, &rec, &why)) {
        // The in-process log stays authoritative for the control-plane
        // reply; a full/missing image section is a builder choice.
        (void)why;
        --audit_seq_;
    }
}

bool Server::install_constraints(ConstraintSet set, std::string* why) {
    Capability coordinator{};
    coordinator.bits = kCapConstraintWrite;
    coordinator.channels = ~0ull;
    coordinator.workspace = workspace_id_;
    const bool ok = constraints_.install(std::move(set), coordinator, &audit_, 0, why);
    if (ok)
        write_constraints_image();
    return ok;
}

void Server::write_constraints_image() {
    if (image_base_ == nullptr)
        return;
    std::string blob;
    const auto put_u32 = [&blob](std::uint32_t v) {
        blob.append(reinterpret_cast<const char*>(&v), sizeof(v));
    };
    for (const auto& rule : constraints_.current().rules) {
        put_u32(static_cast<std::uint32_t>(rule.name.size()));
        put_u32(static_cast<std::uint32_t>(rule.source.size()));
        blob += rule.name;
        blob += rule.source;
    }
    std::string why;
    (void)constraint_write({image_base_, image_size_}, constraints_.version(),
                           {reinterpret_cast<const std::byte*>(blob.data()), blob.size()}, &why);
}

void Server::register_constraint_predicate(std::string name, ConstraintPredicate check) {
    predicate_registry_[std::move(name)] = std::move(check);
}

bool Server::install_policy(const Capability& caps, WorkspacePolicy policy, std::string* why) {
    ConstraintSet next = constraints_.current();
    next.policy = std::move(policy);
    if (!constraints_.install(std::move(next), caps, &audit_, 0, why))
        return false;
    write_constraints_image();
    return true;
}

// ── Shared protocol core (#5377) ─────────────────────────────
// One switch serves both the single-peer harness path and the
// multi-peer coordinator; session state arrives by reference so a
// crash/EOF reclaim never touches another session's ledgers.

void Server::project_worldline(std::uint64_t id, WorldlineState state, ConstraintResult cr) {
    if (image_base_ == nullptr)
        return;
    SectionEntry se{};
    if (!find_section({image_base_, image_size_}, SectionKind::Worldlines, &se))
        return;
    ChannelView view{};
    if (!channel_view({image_base_, image_size_}, Channel::Worldlines, &view))
        return;
    // The section's stride governs the record width: a v1 image keeps
    // 48-byte records, a v2 image carries the evidence tail (#5379).
    const std::uint64_t elem = view.element_size != 0 ? view.element_size : sizeof(WorldlineRecord);
    if (view.length + elem > se.capacity)
        return; // capacity exhaustion stays a hard error on the publish
                // path; the control-plane ledger remains authoritative
    WorldlineRecord w{};
    w.id = id;
    w.parent = kImageNoParent;
    w.state = static_cast<std::uint8_t>(state);
    w.constraint_result = static_cast<std::uint8_t>(cr);
    w.metrics_off = kImageNoIndex;
    if (const TrialRecord* t = worldlines_.find(id))
        w.baseline_gen = t->base_generation;
    if (const auto it = evidence_.find(id); it != evidence_.end()) {
        w.candidate_hash = it->second.candidate_hash;
        w.workload_sym = it->second.workload_sym;
        w.seed = it->second.seed;
        w.verifier_version = it->second.verifier_version;
    }
    if (const auto it = gate_verdicts_.find(id); it != gate_verdicts_.end()) {
        // #5391: the coordinator's admission verdict travels with the
        // worldline record, so a reader can see what the statistics
        // decided without replaying the audit.
        w.gate_verdict = it->second.verdict;
        w.gate_family = it->second.family;
        w.gate_adjusted_ppm = static_cast<std::uint64_t>(it->second.adjusted_ppm +
                                                         static_cast<std::int64_t>(kGateBias));
        w.gate_threshold_ppm = it->second.threshold_ppm;
        w.gate_fingerprint = it->second.fingerprint;
    }
    std::memcpy(image_base_ + view.offset + view.length, &w,
                static_cast<std::size_t>(std::min<std::uint64_t>(elem, sizeof(w))));
    std::uint64_t gen = 0;
    std::string why;
    if (channel_commit({image_base_, image_size_}, Channel::Worldlines, view.offset + view.length,
                       elem, &gen, &why)) {
        projected_trials_ = id;
        generation_ = gen;
    }
}

std::vector<PatchRecord> Server::trial_patches(std::uint64_t worldline) const {
    std::vector<PatchRecord> out;
    if (image_base_ == nullptr)
        return out;
    ChannelView view{};
    if (!channel_view({image_base_, image_size_}, Channel::Patches, &view))
        return out;
    const std::uint64_t elem = view.element_size != 0 ? view.element_size : sizeof(PatchRecord);
    for (std::uint64_t i = 0; i < view.count; ++i) {
        PatchRecord p{};
        std::memcpy(&p, image_base_ + view.offset + i * elem,
                    static_cast<std::size_t>(std::min<std::uint64_t>(elem, sizeof(p))));
        if (p.worldline == worldline)
            out.push_back(p);
    }
    return out;
}

std::uint64_t Server::candidate_digest(std::uint64_t worldline) const {
    std::uint64_t h = 14695981039346656037ull;
    for (const PatchRecord& p : trial_patches(worldline)) {
        const auto* bytes = reinterpret_cast<const std::byte*>(&p);
        for (std::size_t i = 0; i < sizeof(p); ++i) {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i]));
            h *= 1099511628211ull;
        }
    }
    // #5379: truncate to the low 62 bits — the fixnum-safe immediate
    // domain (bit 63 clear, and the value fits make_int's 62-bit range).
    return h & ((1ull << 62) - 1);
}

std::uint64_t Server::candidate_content_digest(std::uint64_t worldline) const {
    const std::vector<PatchRecord> patches = trial_patches(worldline);
    if (patches.empty())
        return 0;
    std::uint64_t h = 14695981039346656037ull;
    const auto hash_u64 = [&h](std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            h ^= (value >> (i * 8)) & 0xffu;
            h *= 1099511628211ull;
        }
    };
    for (const PatchRecord& p : patches) {
        // The target and the field name the change; the string table
        // index does not (it is not stable across hosts).
        hash_u64(p.node);
        hash_u64(p.field_offset);
        if (p.field_offset == kPatchSourceField) {
            std::string_view text;
            if (string_table_read({image_base_, image_size_}, p.new_value, &text)) {
                for (const char ch : text) {
                    h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
                    h *= 1099511628211ull;
                }
            } else {
                hash_u64(p.new_value);
            }
        } else {
            hash_u64(p.new_value);
        }
    }
    return h & ((1ull << 62) - 1);
}

const EvidenceBody* Server::evidence(std::uint64_t worldline) const {
    const auto it = evidence_.find(worldline);
    return it == evidence_.end() ? nullptr : &it->second;
}

void Server::notify_peers(std::uint64_t except_session) {
    const std::uint64_t one = 1;
    for (const auto& peer : peers_) {
        if (peer.session == except_session || peer.event_fd < 0)
            continue;
        ssize_t ignored = ::write(peer.event_fd, &one, sizeof(one));
        (void)ignored;
    }
}

// #5381: a KEEP with an observation window enters Canary instead of
// sealing immediately. At most one canary is active at a time. The
// live apply already happened; the generation advances only at seal,
// so a revert leaves the baseline untouched.
void Server::canary_on_keep(std::uint64_t worldline) {
    if (canary_window_ == 0)
        return;
    std::string why;
    if (!worldlines_.enter_canary(worldline, &why))
        return;
    canary_worldline_ = worldline;
    canary_ticks_ = 0;
    canary_below_ = 0;
    canary_applied_gen_ = workspace_gen_ + 1;
    record_audit(AuditKind::CanaryStart, 0, worldline, canary_window_);
    project_worldline(worldline, WorldlineState::Canary, ConstraintResult::Pass);
}

void Server::canary_seal(std::uint64_t worldline, std::uint64_t token) {
    std::string why;
    if (!worldlines_.seal_canary(worldline, &why))
        return;
    record_audit(AuditKind::CanarySeal, token, worldline, canary_ticks_);
    project_worldline(worldline, WorldlineState::Kept, ConstraintResult::Pass);
    workspace_gen_ = canary_applied_gen_;
    canary_worldline_ = 0;
    // #5393: the observation sealed, the round is done.
    round_note_canary_seal(worldline);
}

void Server::canary_revert(std::uint64_t worldline, std::string_view reason) {
    std::string why;
    if (!worldlines_.revert_canary(worldline, &why))
        return;
    canary_worldline_ = 0;
    if (revert_) {
        std::string rwhy;
        (void)revert_(worldline, &rwhy);
    }
    audit_.add_constraint(AuditKind::CanaryRevert, 0, worldline, 0, constraints_.version(),
                          std::string(reason));
    // #5391: the observation proved the candidate harmful — the KEEP
    // that carried it is a negative KEEP.
    note_negative_keep(worldline);
    // #5393: the round fails closed.
    round_roll("canary reverted: the candidate did not hold up");
    project_worldline(worldline, WorldlineState::Rolled, ConstraintResult::Pass);
}

void Server::canary_check_metric(std::uint64_t key, std::uint64_t value) {
    if (canary_worldline_ == 0 || key != canary_metric_key_)
        return;
    ++canary_ticks_;
    if (canary_floor_ != 0 && value < canary_floor_) {
        ++canary_below_;
        // Two consecutive samples under the floor: distribution drift,
        // not a single noisy tick — revert automatically (#5381).
        if (canary_below_ >= 2) {
            canary_revert(canary_worldline_, "canary-reverted: metric below floor");
            return;
        }
    } else {
        canary_below_ = 0;
    }
    if (canary_window_ != 0 && canary_ticks_ >= canary_window_) {
        // Window expired with the last sample under the floor: the
        // observation did not prove the candidate — fail closed.
        if (canary_floor_ != 0 && canary_below_ > 0) {
            canary_revert(canary_worldline_, "canary-reverted: window expired below floor");
            return;
        }
        canary_seal(canary_worldline_, 0);
    }
}

// ── Statistical admission (#5391) ─────────────────────────────
// The coordinator recomputes the judge's numbers from the bound
// windows: a summary that flatters a candidate buys nothing, and the
// multiple-comparison correction uses the family the coordinator
// observed itself, not the one the judge declares.

const GateEvidenceBody* Server::gate_evidence(std::uint64_t worldline) const {
    const auto it = gate_evidence_.find(worldline);
    return it == gate_evidence_.end() ? nullptr : &it->second;
}

const GateVerdictRecord* Server::gate_verdict(std::uint64_t worldline) const {
    const auto it = gate_verdicts_.find(worldline);
    return it == gate_verdicts_.end() ? nullptr : &it->second;
}

std::uint64_t Server::gate_family_size(std::uint64_t round_id) const {
    const auto it = gate_rounds_.find(round_id);
    return it == gate_rounds_.end() ? 0 : static_cast<std::uint64_t>(it->second.size());
}

bool Server::gate_round_has_control(std::uint64_t round_id) const {
    const auto it = gate_rounds_.find(round_id);
    if (it == gate_rounds_.end())
        return false;
    for (const std::uint64_t wl : it->second) {
        const auto cit = gate_evidence_.find(wl);
        if (cit != gate_evidence_.end() && cit->second.control != 0)
            return true;
    }
    return false;
}

void Server::gate_bind_round(std::uint64_t round_id, std::uint64_t worldline) {
    auto& members = gate_rounds_[round_id];
    if (std::find(members.begin(), members.end(), worldline) == members.end())
        members.push_back(worldline);
    if (std::find(gate_round_order_.begin(), gate_round_order_.end(), round_id) ==
        gate_round_order_.end())
        gate_round_order_.push_back(round_id);
    // A round that left the window cannot gain members: the family it
    // was judged with stays fixed.
    while (gate_round_order_.size() > 64) {
        const std::uint64_t oldest = gate_round_order_.front();
        gate_round_order_.erase(gate_round_order_.begin());
        gate_rounds_.erase(oldest);
    }
}

void Server::note_negative_keep(std::uint64_t worldline) {
    const auto it = gate_keep_slots_.find(worldline);
    if (it == gate_keep_slots_.end())
        return;
    negative_keeps_.mark_negative(it->second);
    gate_tighten_if_needed();
}

void Server::note_negative_event() {
    negative_keeps_.record_negative();
    gate_tighten_if_needed();
}

void Server::gate_tighten_if_needed() {
    if (!gate_policy_.required)
        return;
    if (!negative_keeps_.over_budget(gate_policy_.negative_budget_ppm))
        return;
    // The negative-KEEP rate is over budget: the admission tightens.
    // The haircut multiplier is scale free, so it bites whatever the
    // metric is.
    gate_policy_.tighten_factor =
        gate_policy_.tighten_factor >= 64 ? 64 : gate_policy_.tighten_factor * 2;
    ++gate_policy_.version;
    audit_.add_constraint(AuditKind::GateTighten, 0, 0, 0, gate_policy_.version,
                          "gate-tightened: negative-keep-rate " +
                              std::to_string(negative_keeps_.rate_ppm()) + "ppm over budget " +
                              std::to_string(gate_policy_.negative_budget_ppm) + "ppm, factor " +
                              std::to_string(gate_policy_.tighten_factor));
}

bool Server::gate_check(std::uint64_t worldline, std::uint64_t token, std::string* why) {
    const auto it = gate_evidence_.find(worldline);
    if (it == gate_evidence_.end()) {
        if (why != nullptr)
            *why = "validation-failed: missing-gate-evidence";
        return false;
    }
    const GateEvidenceBody& gev = it->second;
    // The statistical evidence must belong to the same evaluation as
    // the bound five-tuple: same workload, same seed, same verifier.
    const EvidenceBody* five = evidence(worldline);
    if (five == nullptr || five->workload_sym != gev.workload_sym || five->seed != gev.seed ||
        five->verifier_version != gev.verifier_version) {
        if (why != nullptr)
            *why = "validation-failed: gate-evidence-mismatch";
        return false;
    }
    const std::uint64_t family = gate_family_size(gev.round_id);
    // Every round carries a negative control the statistics must
    // reject. A control the admission accepts is a hole in the gate:
    // it is counted as a negative event and the round is refused.
    if (gate_policy_.require_control) {
        bool have_control = false;
        bool control_accepted = false;
        const auto rit = gate_rounds_.find(gev.round_id);
        if (rit != gate_rounds_.end()) {
            for (const std::uint64_t wl : rit->second) {
                const auto cit = gate_evidence_.find(wl);
                if (cit == gate_evidence_.end() || cit->second.control == 0)
                    continue;
                have_control = true;
                std::string creason;
                const GateVerdictRecord crec =
                    gate_admit(cit->second, family, gate_policy_, &creason);
                if (crec.verdict == static_cast<std::uint64_t>(GateVerdict::Keep)) {
                    control_accepted = true;
                    audit_.add_constraint(AuditKind::GateVerdict, token, wl, crec.verdict,
                                          gate_policy_.version,
                                          "gate-control-accepted: the admission accepted a "
                                          "negative control");
                }
            }
        }
        if (!have_control) {
            if (why != nullptr)
                *why = "validation-failed: missing-negative-control";
            return false;
        }
        if (control_accepted) {
            note_negative_event();
            if (why != nullptr)
                *why = "validation-failed: negative-control-accepted";
            return false;
        }
    }
    // A negative control is never applied to the live workspace: it
    // exists to prove the statistics reject a harmful candidate.
    if (gev.control != 0) {
        std::string creason;
        const GateVerdictRecord crec = gate_admit(gev, family, gate_policy_, &creason);
        audit_.add_constraint(AuditKind::GateVerdict, token, worldline, crec.verdict,
                              gate_policy_.version, "gate-control-rejected: " + creason);
        if (why != nullptr)
            *why = "validation-failed: negative-control-refused";
        return false;
    }
    std::string reason;
    const GateVerdictRecord rec = gate_admit(gev, family, gate_policy_, &reason);
    gate_verdicts_[worldline] = rec;
    audit_.add_constraint(
        AuditKind::GateVerdict, token, worldline, rec.verdict, gate_policy_.version,
        std::string("gate-") + reason + ": observed " + std::to_string(rec.observed_ppm) +
            "ppm adjusted " + std::to_string(rec.adjusted_ppm) + "ppm family " +
            std::to_string(rec.family));
    if (rec.verdict != static_cast<std::uint64_t>(GateVerdict::Keep)) {
        if (why != nullptr)
            *why = "validation-failed: gate-rejected: " + reason;
        return false;
    }
    return true;
}

// ── Audit replay (#5392) ─────────────────────────────────────
// A read over the control-plane log and the image: it reconstructs
// the lineage, checks its integrity and reproduces the admission
// verdicts from the bound evidence.
ReplayReport Server::replay() const {
    std::vector<ReplayEvidence> evidence;
    evidence.reserve(evidence_.size() + gate_evidence_.size());
    for (const auto& [id, five] : evidence_) {
        ReplayEvidence item{};
        item.worldline = id;
        item.baseline_gen = five.baseline_gen;
        item.candidate_hash = five.candidate_hash;
        item.workload_sym = five.workload_sym;
        item.seed = five.seed;
        item.verifier_version = five.verifier_version;
        item.has_five = true;
        const auto git = gate_evidence_.find(id);
        if (git != gate_evidence_.end()) {
            item.gate = git->second;
            item.has_gate = true;
        }
        const auto vit = gate_verdicts_.find(id);
        if (vit != gate_verdicts_.end()) {
            item.verdict = vit->second;
            item.has_verdict = true;
        }
        evidence.push_back(item);
    }
    // A candidate that never bound the five-tuple can still hold a
    // statistical verdict: the gate binds its own evidence.
    for (const auto& [id, gate] : gate_evidence_) {
        bool known = false;
        for (const ReplayEvidence& item : evidence) {
            if (item.worldline == id) {
                known = true;
                break;
            }
        }
        if (known)
            continue;
        ReplayEvidence item{};
        item.worldline = id;
        item.gate = gate;
        item.has_gate = true;
        const auto vit = gate_verdicts_.find(id);
        if (vit != gate_verdicts_.end()) {
            item.verdict = vit->second;
            item.has_verdict = true;
        }
        evidence.push_back(item);
    }
    const auto digest = [this](std::uint64_t worldline) { return candidate_digest(worldline); };
    return replay_lineage(audit_.events(), {image_base_, image_size_}, evidence,
                          gate_policy_.required, digest);
}

// ── The admission round (#5393) ──────────────────────────────
// One round at a time, host-driven: the peers propose and adjudicate,
// the coordinator decides. Every failure fails closed — the round is
// abandoned and its pending candidate rolls back.

namespace {
    // The wall clock the budget counts. Monotonic: a clock jump must
    // not close a round.
    std::uint64_t round_now_ms() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }

    // The wall clock the audit and the checkpoint record.
    std::uint64_t round_wall_ms() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    }
} // namespace

bool Server::round_open(std::uint64_t session, std::string* why) {
    std::string reason;
    if (!round_.open(round_now_ms(), canary_worldline_ != 0, &reason)) {
        if (why != nullptr)
            *why = reason;
        return false;
    }
    audit_.add_constraint(AuditKind::RoundOpen, session, 0, round_.id(), round_.budget().version,
                          "round-open");
    std::string cwhy;
    (void)round_checkpoint_.record_round(round_.id(), round_.phase(), 0, round_wall_ms(), &cwhy);
    return true;
}

void Server::round_note_trial(std::uint64_t worldline, std::uint64_t owner_session) {
    if (!round_.active())
        return;
    std::string why;
    if (!round_.note_trial(worldline, owner_session, &why))
        return;
    audit_.add_constraint(AuditKind::RoundTrial, owner_session, worldline, round_.id(),
                          round_.budget().version, "round-trial");
    std::string cwhy;
    (void)round_checkpoint_.record_round(round_.id(), round_.phase(), worldline, round_wall_ms(),
                                         &cwhy);
}

void Server::round_note_judge(std::uint64_t session) {
    if (!round_.active())
        return;
    std::string why;
    if (!round_.note_judge(session, &why))
        return;
    audit_.add_constraint(AuditKind::RoundJudge, session, round_.worldline(), round_.id(),
                          round_.budget().version, "round-judge");
}

void Server::round_note_keep(std::uint64_t worldline) {
    if (!round_.active() || round_.worldline() != worldline)
        return;
    std::string why;
    if (!round_.note_keep(&why))
        return;
    audit_.add_constraint(AuditKind::RoundJudge, 0, worldline, round_.id(), round_.budget().version,
                          "round-keep");
}

void Server::round_note_seal(std::uint64_t worldline, bool canary) {
    if (!round_.active() || round_.worldline() != worldline)
        return;
    std::string why;
    if (!round_.note_seal(canary, round_now_ms(), &why))
        return;
    audit_.add_constraint(AuditKind::RoundSeal, 0, worldline, round_.id(), round_.budget().version,
                          canary ? "round-canary" : "round-sealed");
    std::string cwhy;
    (void)round_checkpoint_.record_round(round_.id(), round_.phase(), worldline, round_wall_ms(),
                                         &cwhy);
    if (canary)
        return;
    // The sealed candidate is remembered: a restarted host does not
    // apply it twice.
    // The seal is remembered by the candidate's content, so a
    // restarted host recognizes the same candidate.
    const std::uint64_t content = candidate_content_digest(worldline);
    if (content != 0)
        (void)round_checkpoint_.record_seal(content, worldline, round_wall_ms(), &cwhy);
}

void Server::round_note_canary_seal(std::uint64_t worldline) {
    if (!round_.active() || round_.worldline() != worldline)
        return;
    std::string why;
    if (!round_.note_canary_seal(round_now_ms(), &why))
        return;
    audit_.add_constraint(AuditKind::RoundSeal, 0, worldline, round_.id(), round_.budget().version,
                          "round-canary-sealed");
    std::string cwhy;
    (void)round_checkpoint_.record_round(round_.id(), round_.phase(), worldline, round_wall_ms(),
                                         &cwhy);
    // The seal is remembered by the candidate's content, so a
    // restarted host recognizes the same candidate.
    const std::uint64_t content = candidate_content_digest(worldline);
    if (content != 0)
        (void)round_checkpoint_.record_seal(content, worldline, round_wall_ms(), &cwhy);
}

void Server::round_roll(std::string_view reason) {
    if (!round_.active())
        return;
    const std::uint64_t worldline = round_.worldline();
    // Fail closed: the pending candidate rolls back, nothing is
    // applied.
    if (worldline != 0) {
        const TrialRecord* trial = worldlines_.find(worldline);
        if (trial != nullptr && trial->pending) {
            Capability caps{};
            caps.bits = kCapRollback;
            caps.channels = ~0ull;
            caps.workspace = workspace_id_;
            std::string why;
            if (worldlines_.rollback(worldline, trial->owner, caps, &why)) {
                record_audit(AuditKind::Rollback, trial->owner, worldline);
                project_worldline(worldline, WorldlineState::Rolled, ConstraintResult::None);
            }
        }
    }
    std::string why;
    if (!round_.roll(reason, round_now_ms(), &why))
        return;
    audit_.add_constraint(AuditKind::RoundRoll, 0, worldline, round_.id(), round_.budget().version,
                          std::string("round-roll: ") + std::string(reason));
    std::string cwhy;
    (void)round_checkpoint_.record_round(round_.id(), round_.phase(), 0, round_wall_ms(), &cwhy);
}

void Server::round_budget_check() {
    if (!round_.active() || !round_.expired(round_now_ms()))
        return;
    const std::uint64_t worldline = round_.worldline();
    // The round ran out of time: close it like a judge that never came
    // back.
    round_roll("budget: the round ran out of wall clock");
    audit_.add_constraint(AuditKind::RoundBudget, 0, worldline, round_.id(),
                          round_.budget().version, "round-budget: the wall budget expired");
}

void Server::round_resume() {
    std::string why;
    if (!round_checkpoint_.open(round_checkpoint_path_, &why))
        return;
    // The round counter continues: a restarted host does not reuse the
    // ids of the rounds it already ran.
    round_.restore(round_checkpoint_.round_id());
    if (round_checkpoint_.round_id() != 0) {
        audit_.add_constraint(AuditKind::RoundResume, 0, round_checkpoint_.round_worldline(),
                              round_checkpoint_.round_id(), round_.budget().version,
                              "round-resume: the host continues after its checkpoint");
    }
}

// #5395: the capacity plan — the room left in the image and the
// rounds it still supports. The channel capacities are a function of
// the delivered image (#5387), so the plan follows the image.
CapacityPlan Server::capacity_plan() const {
    CapacityPlan plan{};
    if (image_base_ == nullptr)
        return plan;
    const ImageHeader header = load_header({image_base_, image_size_});
    plan.committed_bytes = header.committed_bytes;
    plan.image_bytes = header.image_size;
    static constexpr Channel kChannels[5] = {Channel::Metrics, Channel::Cct, Channel::Patches,
                                             Channel::Worldlines, Channel::Nodes};
    static constexpr SectionKind kSections[5] = {SectionKind::Metrics, SectionKind::Cct,
                                                 SectionKind::Patches, SectionKind::Worldlines,
                                                 SectionKind::Nodes};
    std::uint64_t rounds = ~0ull;
    for (std::size_t i = 0; i < 5; ++i) {
        ChannelView view{};
        if (!channel_view({image_base_, image_size_}, kChannels[i], &view))
            continue;
        SectionEntry entry{};
        if (!find_section({image_base_, image_size_}, kSections[i], &entry))
            continue;
        const std::uint64_t stride = view.element_size != 0 ? view.element_size : 1;
        const std::uint64_t used = view.count * stride;
        plan.capacity[i] = entry.capacity;
        plan.remaining[i] = entry.capacity > used ? (entry.capacity - used) / stride : 0;
        if (kChannels[i] == Channel::Worldlines)
            rounds = std::min(rounds, plan.remaining[i] / kSloWorldlinesPerRound);
        else if (kChannels[i] == Channel::Patches)
            rounds = std::min(rounds, plan.remaining[i] / kSloPatchesPerRound);
    }
    if (rounds == ~0ull)
        rounds = 0;
    plan.rounds = rounds;
    plan.low = rounds <= kSloLowRounds ? 1 : 0;
    return plan;
}

// #5395: the performance counters and the capacity plan.
bool Server::slo_query(std::uint64_t kind, SloReplyBody* out, std::string* why) {
    SloReplyBody reply{};
    reply.frames = slo_.frames;
    reply.frame_cpu_ns = slo_.frame_cpu_ns;
    reply.p99_cpu_ns = slo_hist_.p99();
    reply.publishes = slo_.publishes;
    reply.publish_cpu_ns = slo_.publish_cpu_ns;
    reply.attaches = slo_.attaches;
    reply.attach_cpu_ns = slo_.attach_cpu_ns;
    reply.catchups = slo_.catchups;
    reply.catchup_max = slo_.catchup_max;
    const CapacityPlan plan = capacity_plan();
    reply.rounds = plan.rounds;
    reply.low = plan.low;
    for (std::size_t i = 0; i < 5; ++i)
        reply.remaining[i] = plan.remaining[i];
    reply.committed_bytes = plan.committed_bytes;
    reply.image_bytes = plan.image_bytes;
    // #5395: the soak lane watches the loans and the pending trials
    // for a residue.
    reply.pending = worldlines_.pending_count();
    std::uint64_t loans = 0;
    for (const PeerSession& peer : peers_)
        loans += peer.loans.outstanding();
    reply.loans = loans;
    (void)kind;
    *out = reply;
    (void)why;
    return true;
}

// #5394: the detector saw a sustained degradation. While an
// observation runs, the degradation belongs to the candidate under
// observation: it is rolled back. Otherwise it is a signal: the host
// opens a round.
void Server::degrade_alert(std::uint64_t key) {
    const std::uint64_t version = degrade_.version();
    if (canary_worldline_ != 0) {
        // #5381: the observation window reuses the detector — a
        // candidate that degrades the metric does not hold up.
        audit_.add_constraint(AuditKind::DegradeAlert, 0, canary_worldline_, key, version,
                              "degrade-alert: the observation degraded");
        canary_revert(canary_worldline_, "canary reverted: the detector saw a degradation");
        return;
    }
    std::string why;
    const bool opened = round_open(0, &why);
    audit_.add_constraint(AuditKind::DegradeAlert, 0, 0, key, version,
                          opened ? "degrade-alert: a round was opened" : "degrade-alert: " + why);
}

// #5396: the frame rate bucket. The session may send `frame_burst`
// frames back to back and `frame_rate` frames per second sustained; a
// peer that wants more drains the bucket and is cut off. The tokens are
// scaled by 1000 so the refill stays exact.
bool Server::allow_frame(PeerSession& peer) {
    if (peer_quota_.frame_rate == 0 || peer_quota_.frame_burst == 0)
        return true;
    const std::uint64_t now_ms = round_now_ms();
    if (peer.frame_refill_ms == 0)
        peer.frame_refill_ms = now_ms;
    const std::uint64_t elapsed = now_ms - peer.frame_refill_ms;
    peer.frame_refill_ms = now_ms;
    peer.frame_tokens += elapsed * peer_quota_.frame_rate;
    const std::uint64_t cap = peer_quota_.frame_burst * 1000;
    if (peer.frame_tokens > cap)
        peer.frame_tokens = cap;
    if (peer.frame_tokens < 1000)
        return false; // the bucket is empty
    peer.frame_tokens -= 1000;
    return true;
}

// #5396: the constraint-change quota. A session that rewrites the gate
// in a loop is throttled; the other sessions are untouched.
bool Server::allow_constraint_change(std::uint64_t session, std::uint64_t root_token) {
    if (peer_quota_.constraint_changes_per_window == 0)
        return true;
    const std::uint64_t now_ms = round_now_ms();
    auto& entry = constraint_windows_[session];
    if (entry.first == 0 || now_ms - entry.first >= peer_quota_.constraint_window_ms) {
        entry.first = now_ms;
        entry.second = 0;
    }
    ++entry.second;
    if (entry.second > peer_quota_.constraint_changes_per_window) {
        audit_.add_constraint(AuditKind::RateLimited, root_token, 0, entry.second,
                              peer_quota_.constraint_changes_per_window,
                              "rate-limited: the session exceeded its constraint change quota");
        return false;
    }
    return true;
}

bool Server::dispatch_frame(int conn_fd, std::uint64_t session, std::uint64_t root_token,
                            Capability& caps, LoanLedger& loans,
                            std::array<LoanLedger, 6>& channel_loans,
                            std::uint64_t staging_capacity, std::uint64_t& staging_used,
                            std::uint64_t* subscribed, const FrameHeader& header,
                            std::span<const std::byte> body) {
    const auto deny = [&](int code, const std::string& why, std::uint64_t worldline = 0) {
        ErrorBody eb{};
        eb.code = static_cast<std::uint32_t>(code);
        const std::size_t n = std::min(why.size(), sizeof(eb.message) - 1);
        std::memcpy(eb.message, why.data(), n);
        send_frame(conn_fd, FrameType::Error, session,
                   {reinterpret_cast<const std::byte*>(&eb), sizeof(eb)});
        // #5392: a refusal names the worldline it refused, so the
        // replay can show it in the lineage.
        audit_.add_constraint(AuditKind::Denied, root_token, worldline, 0, constraints_.version(),
                              why);
    };
    const auto require = [&](std::uint64_t bit, std::uint64_t channel, std::uint64_t worldline) {
        std::string why;
        if (allows(caps, bit, channel, generation_, worldline, &why))
            return true;
        deny(EPERM, why);
        return false;
    };
    const auto loan_from = [&]() {
        LoanBody loan{};
        if (header.len >= sizeof(loan))
            std::memcpy(&loan, body.data(), sizeof(loan));
        return loan;
    };
    const auto reply_loan = [&](const LoanBody& loan) {
        send_frame(conn_fd, FrameType::Credit, session,
                   {reinterpret_cast<const std::byte*>(&loan), sizeof(loan)});
    };

    switch (static_cast<FrameType>(header.type)) {
        case FrameType::Subscribe:
        case FrameType::Unsubscribe:
        case FrameType::Query: {
            const LoanBody loan = loan_from();
            Channel channel{};
            if (channel_from(loan.channel, &channel) && image_base_ != nullptr) {
                if (!require(kCapRead, loan.channel, 0))
                    break;
                ChannelView view{};
                if (!channel_view({image_base_, image_size_}, channel, &view)) {
                    deny(EINVAL, "channel section is missing");
                    break;
                }
                ChannelBody reply{loan.channel, view.generation, view.count,
                                  view.offset,  view.length,     1,
                                  view.count};
                send_frame(conn_fd, FrameType::Credit, session,
                           {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
                if (subscribed != nullptr) {
                    if (header.type == static_cast<std::uint16_t>(FrameType::Subscribe))
                        *subscribed |= 1ull << loan.channel;
                    else if (header.type == static_cast<std::uint16_t>(FrameType::Unsubscribe))
                        *subscribed &= ~(1ull << loan.channel);
                }
                if (header.type == static_cast<std::uint16_t>(FrameType::Subscribe))
                    record_audit(AuditKind::Subscribe, root_token, 0, loan.channel);
                break;
            }
            if (require(kCapRead, loan.channel, 0))
                reply_loan(loan);
            break;
        }
        case FrameType::AuditQuery: {
            if (header.len < sizeof(AuditQueryBody))
                break;
            AuditQueryBody request{};
            std::memcpy(&request, body.data(), sizeof(request));
            if (!require(kCapRead, 0, 0))
                break;
            struct Selected {
                std::uint64_t seq = 0;
                const AuditEvent* event = nullptr;
            };
            std::vector<Selected> events;
            const auto& all = audit_.events();
            events.reserve(all.size());
            for (std::size_t i = 0; i < all.size(); ++i) {
                const std::uint64_t seq = static_cast<std::uint64_t>(i) + 1;
                const AuditEvent& e = all[i];
                if (request.since_seq != 0 && seq < request.since_seq)
                    continue;
                if (request.worldline != 0 && e.worldline != request.worldline)
                    continue;
                if (request.actor != 0 && e.token != request.actor)
                    continue;
                events.push_back({seq, &e});
            }
            constexpr std::size_t kMaxEvents = 20;
            const std::size_t count = std::min(events.size(), kMaxEvents);
            std::vector<std::byte> out(sizeof(AuditReply) + count * sizeof(AuditWire));
            AuditReply reply{};
            reply.count = count;
            reply.truncated = events.size() > count ? 1 : 0;
            std::memcpy(out.data(), &reply, sizeof(reply));
            for (std::size_t i = 0; i < count; ++i) {
                AuditWire wire{};
                const AuditEvent& e = *events[i].event;
                wire.kind = static_cast<std::uint64_t>(e.kind);
                wire.token = e.token;
                wire.worldline = e.worldline;
                wire.result = e.result;
                wire.version = e.constraint_version;
                wire.timestamp_ms = e.timestamp_ms;
                wire.seq = events[i].seq;
                const std::size_t n = std::min(e.reason.size(), sizeof(wire.reason) - 1);
                std::memcpy(wire.reason, e.reason.data(), n);
                std::memcpy(out.data() + sizeof(reply) + i * sizeof(AuditWire), &wire,
                            sizeof(wire));
            }
            send_frame(conn_fd, FrameType::Credit, session, out);
            break;
        }
        case FrameType::AuditReplay: {
            // #5392: reconstruct the lineage, check its integrity and
            // reproduce the admission verdicts. A read: it changes
            // nothing.
            if (header.len < sizeof(AuditReplayBody))
                break;
            AuditReplayBody request{};
            std::memcpy(&request, body.data(), sizeof(request));
            if (!require(kCapRead, 0, 0))
                break;
            const ReplayReport report = replay();
            AuditReplayReply reply{};
            reply.events = report.events;
            reply.worldlines = report.worldlines;
            reply.keeps = report.keeps;
            reply.findings = report.findings;
            reply.first_seq = report.first_seq;
            reply.first_kind = report.first_kind;
            reply.run = workspace_id_;
            const std::size_t n = std::min(report.nodes.size(), std::size_t{16});
            reply.truncated = report.nodes.size() > n ? 1 : 0;
            const std::size_t reason_len =
                std::min(report.first_reason.size(), sizeof(reply.first_reason) - 1);
            std::memcpy(reply.first_reason, report.first_reason.data(), reason_len);
            std::vector<std::byte> out(sizeof(reply) + n * sizeof(AuditReplayNode));
            std::memcpy(out.data(), &reply, sizeof(reply));
            for (std::size_t i = 0; i < n; ++i) {
                const ReplayNode& node = report.nodes[i];
                AuditReplayNode wire{};
                wire.worldline = node.worldline;
                wire.parent = node.parent;
                wire.state = node.state;
                wire.trial_seq = node.trial_seq;
                wire.keep_seq = node.keep_seq;
                wire.verdict_seq = node.verdict_seq;
                wire.constraint = node.constraint;
                wire.reproduced = node.reproduced;
                wire.fingerprint = node.fingerprint;
                // The reason of the KEEP (or the trial) event, and the
                // evidence the verdict was made from.
                for (const AuditEvent& event : audit_.events()) {
                    if (event.worldline != node.worldline)
                        continue;
                    if (event.kind != AuditKind::Keep && event.kind != AuditKind::Rollback &&
                        event.kind != AuditKind::CanaryRevert && event.kind != AuditKind::Trial)
                        continue;
                    wire.timestamp_ms = event.timestamp_ms;
                    const std::size_t len = std::min(event.reason.size(), sizeof(wire.reason) - 1);
                    std::memcpy(wire.reason, event.reason.data(), len);
                    break;
                }
                const auto eit = evidence_.find(node.worldline);
                if (eit != evidence_.end()) {
                    wire.baseline_gen = eit->second.baseline_gen;
                    wire.candidate_hash = eit->second.candidate_hash;
                    wire.workload_sym = eit->second.workload_sym;
                    wire.seed = eit->second.seed;
                    wire.verifier_version = eit->second.verifier_version;
                }
                const auto vit = gate_verdicts_.find(node.worldline);
                if (vit != gate_verdicts_.end()) {
                    wire.gate_verdict = vit->second.verdict;
                    wire.gate_family = vit->second.family;
                    wire.adjusted_ppm = vit->second.adjusted_ppm;
                }
                std::memcpy(out.data() + sizeof(reply) + i * sizeof(wire), &wire, sizeof(wire));
            }
            send_frame(conn_fd, FrameType::Credit, session, out);
            break;
        }
        case FrameType::CatchUp: {
            if (header.len < sizeof(CatchUpBody) || image_base_ == nullptr)
                break;
            CatchUpBody request{};
            std::memcpy(&request, body.data(), sizeof(request));
            Channel channel{};
            if (!channel_from(request.channel, &channel)) {
                deny(EINVAL, "unknown channel");
                break;
            }
            if (!require(kCapRead, request.channel, 0))
                break;
            ChannelView view{};
            if (!channel_view({image_base_, image_size_}, channel, &view)) {
                deny(EINVAL, "channel section is missing");
                break;
            }
            // #5385: generations are image-global (audit and constraint
            // writes advance them too), so a cursor is valid while its
            // generation is at or below the section's and its count fits
            // the append-only channel. Ahead of either — the cursor may
            // predate a rebuild — forces a full snapshot, never a slice.
            const bool snapshot = (request.generation == 0 && request.count == 0) ||
                                  request.generation > view.generation ||
                                  request.count > view.count;
            const std::uint64_t from = snapshot ? 0 : request.count;
            ChannelBody reply{request.channel,
                              view.generation,
                              view.count - from,
                              view.offset + from * view.element_size,
                              (view.count - from) * view.element_size,
                              static_cast<std::uint64_t>(snapshot ? 1 : 0),
                              view.count};
            // #5395: the catchup increment size (the largest one the
            // coordinator ever handed out).
            ++slo_.catchups;
            if (reply.length > slo_.catchup_max)
                slo_.catchup_max = reply.length;
            send_frame(conn_fd, FrameType::Credit, session,
                       {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
            break;
        }
        case FrameType::Reserve: {
            const LoanBody loan = loan_from();
            Channel channel{};
            if (channel_from(loan.channel, &channel) && image_base_ != nullptr) {
                if (!require(kCapPublish, loan.channel, 0))
                    break;
                auto& ledger = channel_loans[static_cast<std::size_t>(channel)];
                if (!ledger.reserve()) {
                    deny(EAGAIN, "channel loan quota exhausted");
                    break;
                }
                std::uint64_t offset = 0;
                std::string why;
                if (!channel_reserve({image_base_, image_size_}, channel, loan.length, &offset,
                                     &why)) {
                    ledger.release();
                    deny(ENOSPC, why);
                    break;
                }
                LoanBody granted = loan;
                granted.offset = offset;
                granted.generation = generation_;
                reply_loan(granted);
                break;
            }
            if (!require(kCapPublish, loan.channel, 0))
                break;
            if (!loans.reserve()) {
                deny(EAGAIN, "loan quota exhausted");
                break;
            }
            if (loan.length == 0 || loan.length > staging_capacity - staging_used) {
                loans.release();
                deny(ENOSPC, "staging window exhausted");
                break;
            }
            LoanBody granted = loan;
            granted.offset = staging_used;
            granted.generation = generation_;
            staging_used += loan.length;
            reply_loan(granted);
            break;
        }
        case FrameType::Publish: {
            const LoanBody loan = loan_from();
            Channel channel{};
            if (channel_from(loan.channel, &channel) && image_base_ != nullptr) {
                if (!require(kCapPublish, loan.channel, loan.worldline))
                    break;
                // #5380: a patch may only name a pending trial, never a
                // settled worldline or an id that was never opened.
                if (loan.worldline != 0 && loan.worldline != kImageNoIndex) {
                    const TrialRecord* t = worldlines_.find(loan.worldline);
                    if (t == nullptr || !t->pending) {
                        deny(EPERM, "patch worldline is not a pending trial");
                        break;
                    }
                }
                // #5378: a source-carrier patch names a string-table
                // entry; validate the index before the record commits.
                if (channel == Channel::Patches) {
                    const std::uint64_t elem = sizeof(PatchRecord);
                    const std::uint64_t n = loan.length / elem;
                    bool bad_source = false;
                    for (std::uint64_t i = 0; i < n; ++i) {
                        PatchRecord p{};
                        std::memcpy(&p, image_base_ + loan.offset + i * elem, sizeof(p));
                        if (p.field_offset == kPatchSourceField) {
                            std::string_view src;
                            if (!string_table_read({image_base_, image_size_}, p.new_value, &src)) {
                                bad_source = true;
                                break;
                            }
                        }
                    }
                    if (bad_source) {
                        deny(EINVAL, "patch source index outside the string table");
                        break;
                    }
                }
                std::uint64_t gen = 0;
                std::string why;
                // #5395: the publish cost is part of the bus's
                // overhead — CPU time, never the wall clock.
                const std::uint64_t publish_started_ns = slo_thread_cpu_ns();
                if (!channel_commit({image_base_, image_size_}, channel, loan.offset, loan.length,
                                    &gen, &why)) {
                    deny(EINVAL, why);
                    break;
                }
                channel_loans[static_cast<std::size_t>(channel)].release();
                ChannelView view{};
                channel_view({image_base_, image_size_}, channel, &view);
                generation_ = gen;
                record_audit(AuditKind::Publish, root_token, 0, loan.length);
                // #5395: the publish cost, in CPU time.
                ++slo_.publishes;
                slo_.publish_cpu_ns += slo_thread_cpu_ns() - publish_started_ns;
                // #5381: online metrics drive the canary observation. Run
                // the check BEFORE the reply so a revert completes before
                // the publisher's next form observes the workspace.
                if (channel == Channel::Metrics) {
                    const std::uint64_t elem =
                        view.element_size != 0 ? view.element_size : sizeof(MetricRecord);
                    const std::uint64_t n = loan.length / elem;
                    for (std::uint64_t i = 0; i < n; ++i) {
                        MetricRecord m{};
                        std::memcpy(
                            &m, image_base_ + loan.offset + i * elem,
                            static_cast<std::size_t>(std::min<std::uint64_t>(elem, sizeof(m))));
                        // #5394: the detector watches the same metrics.
                        std::uint64_t fired = 0;
                        if (degrade_.observe(m.key_sym, m.value, &fired))
                            degrade_alert(fired);
                        canary_check_metric(m.key_sym, m.value);
                    }
                }
                ChannelBody reply{loan.channel, gen, view.count, view.offset,
                                  view.length,  0,   view.count};
                send_frame(conn_fd, FrameType::Credit, session,
                           {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
                notify_peers(session);
                break;
            }
            if (!require(kCapPublish, loan.channel, loan.worldline))
                break;
            if (loan.length == 0 || loan.offset > staging_capacity ||
                loan.length > staging_capacity - loan.offset) {
                deny(EINVAL, "publish range outside the staging window");
                break;
            }
            reply_loan(loan);
            break;
        }
        case FrameType::Credit: {
            const LoanBody loan = loan_from();
            Channel channel{};
            if (channel_from(loan.channel, &channel) && image_base_ != nullptr)
                channel_loans[static_cast<std::size_t>(channel)].release();
            else
                loans.release();
            break;
        }
        case FrameType::Trial: {
            const LoanBody loan = loan_from();
            if (!require(kCapTrial, loan.channel, 0))
                break;
            // #5378: bind the trial to the live workspace generation it
            // starts from; KEEP rejects a stale baseline.
            const std::uint64_t id =
                worldlines_.open_trial(root_token, workspace_id_, workspace_gen_);
            record_audit(AuditKind::Trial, root_token, id);
            // #5393: the trial joins the round when one is running.
            round_note_trial(id, session);
            project_worldline(id, WorldlineState::Pending, ConstraintResult::None);
            notify_peers(session);
            LoanBody granted = loan;
            granted.worldline = id;
            granted.generation = generation_;
            reply_loan(granted);
            break;
        }
        case FrameType::SourcePublish: {
            // #5378: the body is raw UTF-8 source text. Append it to the
            // string table and return the entry index; the proposer then
            // publishes a patch with field_offset=kPatchSourceField and
            // new_value=index. Needs publish; the text is committed evidence.
            if (!require(kCapPublish, 0, 0))
                break;
            if (image_base_ == nullptr) {
                deny(EINVAL, "no image to hold the source");
                break;
            }
            const std::string_view text(reinterpret_cast<const char*>(body.data()), header.len);
            std::uint64_t index = 0;
            std::string why;
            if (!string_table_append({image_base_, image_size_}, text, &index, &why)) {
                deny(ENOSPC, why);
                break;
            }
            generation_ = load_header({image_base_, image_size_}).committed_gen;
            record_audit(AuditKind::Publish, root_token, 0, header.len);
            notify_peers(session);
            LoanBody ok{};
            ok.offset = index;
            ok.generation = generation_;
            reply_loan(ok);
            break;
        }
        case FrameType::EvidenceBind: {
            // #5379: bind the five-tuple to a pending trial. Only the
            // owner or a capability scoped to that worldline may bind.
            if (header.len < sizeof(EvidenceBody)) {
                deny(EINVAL, "evidence body is too short");
                break;
            }
            EvidenceBody ev{};
            std::memcpy(&ev, body.data(), sizeof(ev));
            if (!require(kCapTrial, 0, ev.worldline))
                break;
            const TrialRecord* t = worldlines_.find(ev.worldline);
            if (t == nullptr || !t->pending) {
                deny(EPERM, "evidence worldline is not a pending trial");
                break;
            }
            if (t->owner != root_token && caps.worldline != ev.worldline) {
                deny(EPERM, "worldline is owned by another session");
                break;
            }
            evidence_[ev.worldline] = ev;
            record_audit(AuditKind::Publish, root_token, ev.worldline, ev.candidate_hash);
            LoanBody ok{};
            ok.worldline = ev.worldline;
            ok.generation = generation_;
            reply_loan(ok);
            break;
        }
        case FrameType::GateBind: {
            // #5391: bind the judge's held-out evidence to a pending
            // trial. The owner or a capability scoped to that worldline
            // may bind; the coordinator keeps the raw windows and
            // recomputes the admission verdict from them.
            if (header.len < sizeof(GateEvidenceBody)) {
                deny(EINVAL, "gate evidence body is too short");
                break;
            }
            GateEvidenceBody gev{};
            std::memcpy(&gev, body.data(), sizeof(gev));
            if (!require(kCapKeep, 0, gev.worldline))
                break;
            const TrialRecord* t = worldlines_.find(gev.worldline);
            if (t == nullptr || !t->pending) {
                deny(EPERM, "gate worldline is not a pending trial");
                break;
            }
            if (t->owner != root_token && caps.worldline != gev.worldline) {
                deny(EPERM, "worldline is owned by another session");
                break;
            }
            if (gev.fold_count == 0 || gev.fold_count > kGateMaxWindows) {
                deny(EINVAL, "validation-failed: gate-fold-count");
                break;
            }
            bool window_ok = true;
            for (std::size_t i = 0; i < static_cast<std::size_t>(gev.fold_count); ++i) {
                const std::int64_t w = static_cast<std::int64_t>(gev.windows[i]);
                if (w > kGateMaxWindowPpm || w < -kGateMaxWindowPpm) {
                    window_ok = false;
                    break;
                }
            }
            if (!window_ok) {
                deny(EINVAL, "validation-failed: gate-window-out-of-range");
                break;
            }
            gate_evidence_[gev.worldline] = gev;
            gate_bind_round(gev.round_id, gev.worldline);
            record_audit(AuditKind::GateBind, root_token, gev.worldline, gev.fold_count);
            notify_peers(session);
            LoanBody ok{};
            ok.worldline = gev.worldline;
            ok.generation = generation_;
            reply_loan(ok);
            break;
        }
        case FrameType::CandidateDigest: {
            // #5379: the proposer/judge reads the coordinator's FNV-1a
            // digest of the trial's committed patch bytes, to bind it as
            // candidate_hash. Needs read; no state change.
            const LoanBody loan = loan_from();
            if (!require(kCapRead, 0, loan.worldline))
                break;
            LoanBody ok{};
            ok.worldline = loan.worldline;
            ok.offset = candidate_digest(loan.worldline);
            ok.generation = generation_;
            reply_loan(ok);
            break;
        }
        case FrameType::CanaryConfirm: {
            // #5381: the judge seals an active observation early.
            const LoanBody loan = loan_from();
            if (!require(kCapKeep, loan.channel, loan.worldline))
                break;
            if (canary_worldline_ != loan.worldline) {
                deny(EPERM, "worldline is not the active canary");
                break;
            }
            canary_seal(loan.worldline, root_token);
            notify_peers(session);
            reply_loan(loan);
            break;
        }
        case FrameType::Keep:
        case FrameType::Rollback: {
            const LoanBody loan = loan_from();
            const bool keep = header.type == static_cast<std::uint16_t>(FrameType::Keep);
            if (!require(keep ? kCapKeep : kCapRollback, loan.channel, loan.worldline))
                break;
            const TrialRecord* trial = worldlines_.find(loan.worldline);
            bool applied_live = false;
            if (keep) {
                // #5393: the candidate is being adjudicated.
                round_note_keep(loan.worldline);
                // #5393: a sealed candidate is never applied twice, not
                // even after the host restarted. The identity is the
                // candidate's content, not its patch bytes.
                const std::uint64_t content = candidate_content_digest(loan.worldline);
                if (content != 0 && round_checkpoint_.sealed(content)) {
                    deny(EINVAL, "validation-failed: already-sealed", loan.worldline);
                    break;
                }
                // #5380: resolve the trial's patches + source texts up
                // front so the constraint gate can inspect them (purity,
                // target allowlist) before anything touches the live AST.
                const std::vector<PatchRecord> patches = trial_patches(loan.worldline);
                std::vector<std::string> sources;
                sources.reserve(patches.size());
                bool has_source = false;
                bool missing_source = false;
                for (const auto& p : patches) {
                    if (p.field_offset == kPatchSourceField) {
                        has_source = true;
                        std::string_view text;
                        if (!string_table_read({image_base_, image_size_}, p.new_value, &text)) {
                            missing_source = true;
                            break;
                        }
                        sources.emplace_back(text);
                    } else {
                        sources.emplace_back();
                    }
                }
                if (missing_source) {
                    deny(EINVAL, "validation-failed: patch source missing from string table",
                         loan.worldline);
                    break;
                }
                ConstraintContext ctx{};
                ctx.worldline = loan.worldline;
                ctx.node = loan.offset;
                ctx.new_value = loan.length;
                ctx.patches = patches;
                ctx.sources = sources;
                const GateResult gate = constraints_.check(ctx, &audit_, root_token);
                if (!gate.pass) {
                    deny(EPERM, "constraint: " + gate.reason, loan.worldline);
                    break;
                }
                // #5381: one observation at a time; a second canary while
                // the first is unsealed would stack rollback snapshots.
                if (canary_window_ != 0 && canary_worldline_ != 0) {
                    deny(EBUSY, "canary-active: seal or revert the active observation first",
                         loan.worldline);
                    break;
                }
                // #5379: evidence gate — a KEEP must carry a matching
                // five-tuple; stale baselines and forged candidate hashes
                // are rejected before anything touches the live AST.
                if (evidence_required_) {
                    const auto eit = evidence_.find(loan.worldline);
                    if (eit == evidence_.end()) {
                        deny(EINVAL, "validation-failed: missing-evidence", loan.worldline);
                        break;
                    }
                    const EvidenceBody& ev = eit->second;
                    if (ev.workload_sym == 0 || ev.seed == 0 || ev.verifier_version == 0) {
                        deny(EINVAL, "validation-failed: incomplete-evidence", loan.worldline);
                        break;
                    }
                    if (trial != nullptr && ev.baseline_gen != trial->base_generation) {
                        deny(EINVAL, "validation-failed: stale-baseline", loan.worldline);
                        break;
                    }
                    if (ev.baseline_gen != workspace_gen_) {
                        deny(EINVAL, "validation-failed: stale-baseline", loan.worldline);
                        break;
                    }
                    if (ev.candidate_hash != candidate_digest(loan.worldline)) {
                        deny(EINVAL, "validation-failed: candidate-hash-mismatch", loan.worldline);
                        break;
                    }
                }
                // #5391: statistical admission — the coordinator
                // recomputes the deflated statistic from the bound
                // windows, corrects it for the round family and
                // refuses anything that does not clear the bar. The
                // workspace is untouched when the gate refuses.
                if (gate_policy_.required) {
                    std::string gwhy;
                    if (!gate_check(loan.worldline, root_token, &gwhy)) {
                        deny(EINVAL, gwhy, loan.worldline);
                        break;
                    }
                }
                // #5378: apply to the live workspace BEFORE the ledger
                // marks Kept. The callback is all-or-nothing; on failure
                // the workspace is byte-identical and the trial stays
                // pending (a later retry or disconnect rollback).
                if (apply_ && has_source) {
                    if (trial != nullptr && trial->base_generation != workspace_gen_) {
                        deny(EINVAL, "validation-failed: stale-baseline", loan.worldline);
                        break;
                    }
                    std::string why;
                    if (!apply_(loan.worldline, workspace_gen_, patches, sources,
                                canary_window_ != 0, &why)) {
                        deny(EINVAL, "validation-failed: " + why, loan.worldline);
                        break;
                    }
                    applied_live = true;
                }
            }
            std::string why;
            bool ok = false;
            if (keep) {
                ok = worldlines_.keep(loan.worldline, root_token, caps, &why, applied_live);
            } else if (canary_worldline_ == loan.worldline) {
                // #5381: rolling back the active canary restores the
                // pre-observation snapshot (judge-initiated revert).
                canary_revert(loan.worldline, "canary-reverted: judge rollback");
                ok = true;
            } else if (trial != nullptr && trial->kept && trial->applied_live && apply_) {
                // A sealed worldline the judge reverts: restore the live
                // workspace and step the generation back (#5381).
                ok = worldlines_.rollback_kept(loan.worldline, root_token, caps, &why);
                if (ok) {
                    if (revert_) {
                        std::string rwhy;
                        (void)revert_(loan.worldline, &rwhy);
                    }
                    if (workspace_gen_ > 0)
                        --workspace_gen_;
                    // #5391: the judge reverted a kept worldline — the
                    // KEEP was a negative one.
                    note_negative_keep(loan.worldline);
                }
            } else {
                ok = worldlines_.rollback(loan.worldline, root_token, caps, &why);
            }
            if (!ok) {
                deny(EPERM, why);
                break;
            }
            if (keep) {
                record_audit(AuditKind::Keep, root_token, loan.worldline);
                // #5391: the KEEP enters the negative-KEEP window. A
                // later revert marks it negative and feeds the budget.
                if (gate_policy_.required)
                    gate_keep_slots_[loan.worldline] = negative_keeps_.record_positive();
                if (canary_window_ != 0) {
                    canary_on_keep(loan.worldline);
                    // #5393: the candidate is under observation.
                    round_note_seal(loan.worldline, true);
                } else {
                    project_worldline(loan.worldline, WorldlineState::Kept, ConstraintResult::Pass);
                    if (applied_live)
                        ++workspace_gen_;
                    // #5393: the round is sealed.
                    round_note_seal(loan.worldline, false);
                }
            } else if (canary_worldline_ != loan.worldline) {
                record_audit(AuditKind::Rollback, root_token, loan.worldline);
                project_worldline(loan.worldline, WorldlineState::Rolled, ConstraintResult::None);
            }
            notify_peers(session);
            reply_loan(loan);
            break;
        }
        case FrameType::DefineQuery: {
            // #5397: the host's live define names. The node ids belong
            // to the host's workspace AST, so a peer that wants to
            // rewrite a named function (choose-fn + admit-fn + ttl-fn)
            // asks the host instead of guessing an id.
            if (!require(kCapRead, 0, 0))
                break;
            if (!define_nodes_) {
                deny(EINVAL, "the host does not publish its define names");
                break;
            }
            std::vector<std::pair<std::string, std::uint64_t>> names;
            if (!define_nodes_(&names)) {
                deny(EINVAL, "the host could not read its define names");
                break;
            }
            // The frame body is bounded; the reply carries what fits and
            // says how many it dropped.
            constexpr std::size_t kMaxDefines = 64;
            std::size_t count = 0;
            std::size_t bytes = sizeof(DefineReply);
            std::vector<std::byte> body;
            body.reserve(kMaxDefines * sizeof(DefineWire));
            for (const auto& [name, node] : names) {
                if (count >= kMaxDefines)
                    break;
                if (name.size() >= sizeof(DefineWire{}.name))
                    continue;
                DefineWire wire{};
                std::memcpy(wire.name, name.data(), name.size());
                wire.node = node;
                const std::size_t at = body.size();
                body.resize(at + sizeof(wire));
                std::memcpy(body.data() + at, &wire, sizeof(wire));
                ++count;
            }
            bytes = sizeof(DefineReply) + body.size();
            std::vector<std::byte> out(bytes);
            DefineReply reply{};
            reply.count = count;
            reply.truncated = names.size() > count ? 1 : 0;
            std::memcpy(out.data(), &reply, sizeof(reply));
            if (!body.empty())
                std::memcpy(out.data() + sizeof(reply), body.data(), body.size());
            send_frame(conn_fd, FrameType::Credit, session,
                       {reinterpret_cast<const std::byte*>(out.data()), out.size()});
            break;
        }
        case FrameType::ReproQuery: {
            // #5397: the reproduction package of one worldline. It
            // carries the evidence five-tuple, the held-out windows, the
            // policy snapshot and the verdict, so the admission decision
            // can be re-run offline and must land on the same verdict.
            if (header.len < sizeof(std::uint64_t)) {
                deny(EINVAL, "repro query needs a worldline id");
                break;
            }
            std::uint64_t worldline = 0;
            std::memcpy(&worldline, body.data(), sizeof(worldline));
            if (!require(kCapRead, 0, 0))
                break;
            ReproPackage package{};
            const auto five = evidence(worldline);
            if (five != nullptr) {
                package.five = *five;
                package.has_five = 1;
            }
            const auto gate = gate_evidence(worldline);
            if (gate != nullptr) {
                package.gate = *gate;
                package.has_gate = 1;
            }
            const auto vit = gate_verdicts_.find(worldline);
            if (vit != gate_verdicts_.end()) {
                package.verdict = vit->second;
                package.has_verdict = 1;
            }
            package.policy = gate_policy_;
            send_frame(conn_fd, FrameType::Credit, session,
                       {reinterpret_cast<const std::byte*>(&package), sizeof(package)});
            break;
        }
        case FrameType::RoundOpen: {
            // #5393: a peer asks the coordinator to open a round. The
            // host decides: one round at a time, no round on top of an
            // unsealed observation, outside the cooldown, under the
            // round cap.
            if (header.len < sizeof(RoundRequestBody))
                break;
            RoundRequestBody request{};
            std::memcpy(&request, body.data(), sizeof(request));
            if (!require(kCapTrial, 0, 0))
                break;
            std::string why;
            if (!round_open(session, &why)) {
                deny(EBUSY, "round: " + why, round_.worldline());
                break;
            }
            RoundReplyBody reply{};
            reply.phase = static_cast<std::uint64_t>(round_.phase());
            reply.round_id = round_.id();
            send_frame(conn_fd, FrameType::Credit, session,
                       {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
            notify_peers(session);
            break;
        }
        case FrameType::RoundQuery: {
            // #5393: the round state and its budget. A read.
            if (header.len < sizeof(RoundRequestBody))
                break;
            RoundRequestBody request{};
            std::memcpy(&request, body.data(), sizeof(request));
            if (!require(kCapRead, 0, 0))
                break;
            RoundReplyBody reply{};
            reply.phase = static_cast<std::uint64_t>(round_.phase());
            reply.round_id = round_.id();
            reply.worldline = round_.worldline();
            reply.opened = round_.opened_count();
            reply.sealed = round_.sealed_count();
            reply.wall_ms = round_.budget().wall_ms;
            reply.cooldown_ms = round_.budget().cooldown_ms;
            reply.max_rounds = round_.budget().max_rounds;
            reply.version = round_.budget().version;
            const std::size_t n = std::min(round_.reason().size(), sizeof(reply.reason) - 1);
            std::memcpy(reply.reason, round_.reason().data(), n);
            send_frame(conn_fd, FrameType::Credit, session,
                       {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
            break;
        }
        case FrameType::DegradeQuery: {
            // #5394: the detector's state and its configuration. A read.
            if (header.len < sizeof(DegradeQueryBody))
                break;
            DegradeQueryBody request{};
            std::memcpy(&request, body.data(), sizeof(request));
            if (!require(kCapRead, 0, 0))
                break;
            const DegradeConfig& config = degrade_.config();
            DegradeReplyBody reply{};
            reply.version = config.version;
            reply.samples = degrade_.samples();
            reply.alerts = degrade_.alerts();
            reply.last_change = degrade_.last_change();
            reply.key_count = degrade_.states().size();
            reply.min_samples = config.min_samples;
            reply.ewma_alpha_ppm = config.ewma_alpha_ppm;
            reply.drift_ppm = config.drift_ppm;
            reply.limit_ppm = config.limit_ppm;
            reply.calm_samples = config.calm_samples;
            const std::size_t n = std::min(degrade_.states().size(), std::size_t{8});
            std::vector<std::byte> out(sizeof(reply) + n * sizeof(DegradeKeyStateBody));
            std::memcpy(out.data(), &reply, sizeof(reply));
            for (std::size_t i = 0; i < n; ++i) {
                const DegradeKeyState& state = degrade_.states()[i];
                DegradeKeyStateBody wire{};
                wire.key = state.key;
                wire.higher_is_worse = state.higher_is_worse ? 1 : 0;
                wire.samples = state.samples;
                wire.alerts = state.alerts;
                wire.ewma = state.ewma;
                wire.cusum_ppm = state.cusum_ppm;
                std::memcpy(out.data() + sizeof(reply) + i * sizeof(wire), &wire, sizeof(wire));
            }
            send_frame(conn_fd, FrameType::Credit, session, out);
            break;
        }
        case FrameType::DegradeConfig: {
            // #5394: install a new detector configuration or roll the
            // previous one back. The configuration is a policy: it needs
            // constraint-write, and every change is audited with its
            // version.
            if (header.len < sizeof(DegradeConfigBody))
                break;
            DegradeConfigBody request{};
            std::memcpy(&request, body.data(), sizeof(request));
            if (!require(kCapConstraintWrite, 0, 0))
                break;
            if ((request.flags & 1) != 0) {
                // The rollback: the previous configuration returns.
                if (!degrade_.rollback()) {
                    deny(EPERM, "degrade: there is no previous configuration");
                    break;
                }
                const DegradeConfig& config = degrade_.config();
                audit_.add_constraint(AuditKind::DegradeConfig, root_token, 0, config.version,
                                      config.version, "degrade-config: rolled back");
                DegradeReplyBody reply{};
                reply.version = config.version;
                reply.min_samples = config.min_samples;
                reply.ewma_alpha_ppm = config.ewma_alpha_ppm;
                reply.drift_ppm = config.drift_ppm;
                reply.limit_ppm = config.limit_ppm;
                reply.calm_samples = config.calm_samples;
                reply.key_count = config.keys.size();
                send_frame(conn_fd, FrameType::Credit, session,
                           {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
                break;
            }
            if (request.key_count > 16 ||
                header.len <
                    sizeof(DegradeConfigBody) + request.key_count * sizeof(DegradeKeyBody)) {
                deny(EINVAL, "degrade: the configuration body is too short");
                break;
            }
            DegradeConfig config{};
            config.min_samples = request.min_samples;
            config.ewma_alpha_ppm = request.ewma_alpha_ppm;
            config.drift_ppm = request.drift_ppm;
            config.limit_ppm = request.limit_ppm;
            config.calm_samples = request.calm_samples;
            config.version = degrade_.version() + 1;
            for (std::uint64_t i = 0; i < request.key_count; ++i) {
                DegradeKeyBody key{};
                std::memcpy(&key, body.data() + sizeof(DegradeConfigBody) + i * sizeof(key),
                            sizeof(key));
                DegradeKey watched{};
                watched.key = key.key;
                watched.higher_is_worse = key.higher_is_worse != 0;
                config.keys.push_back(watched);
            }
            degrade_.install(config);
            audit_.add_constraint(AuditKind::DegradeConfig, root_token, 0, config.version,
                                  config.version, "degrade-config: installed");
            DegradeReplyBody reply{};
            reply.version = config.version;
            reply.min_samples = config.min_samples;
            reply.ewma_alpha_ppm = config.ewma_alpha_ppm;
            reply.drift_ppm = config.drift_ppm;
            reply.limit_ppm = config.limit_ppm;
            reply.calm_samples = config.calm_samples;
            reply.key_count = config.keys.size();
            send_frame(conn_fd, FrameType::Credit, session,
                       {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
            break;
        }
        case FrameType::SloQuery: {
            // #5395: the performance counters and the capacity plan.
            if (header.len < sizeof(SloQueryBody))
                break;
            SloQueryBody request{};
            std::memcpy(&request, body.data(), sizeof(request));
            if (!require(kCapRead, 0, 0))
                break;
            SloReplyBody reply{};
            std::string why;
            if (!slo_query(request.kind, &reply, &why)) {
                deny(EINVAL, "slo: " + why);
                break;
            }
            send_frame(conn_fd, FrameType::Credit, session,
                       {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
            break;
        }
        case FrameType::Grant: {
            if (header.len < sizeof(GrantBody))
                break;
            GrantBody request{};
            std::memcpy(&request, body.data(), sizeof(request));
            if (!require(kCapGrant, 0, 0))
                break;
            if (request.caps.workspace == 0)
                request.caps.workspace = workspace_id_;
            const std::uint64_t parent = request.parent == 0 ? root_token : request.parent;
            std::string why;
            const auto token = caps_table_.derive(parent, request.caps, &why);
            if (!token) {
                // #5396: a refused grant is audited on its own, so the
                // abuse pattern is visible.
                audit_.add_constraint(AuditKind::GrantLimited, root_token, 0, 0,
                                      caps_table_.record_count(),
                                      std::string("grant-limited: ") + why);
                deny(EPERM, why);
                break;
            }
            GrantReply reply{};
            reply.token = *token;
            send_frame(conn_fd, FrameType::Credit, session,
                       {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
            record_audit(AuditKind::Grant, root_token, 0, *token);
            break;
        }
        case FrameType::GrantActivate: {
            // #5377: the session presents a granted token (loan.channel)
            // to narrow its own capability to the token's effective caps.
            // The judge activates the host's delegated scoped keep+rollback
            // on a single worldline. Only narrowing is allowed — the token
            // must be effective (not revoked) and a subset of the session's
            // current caps is NOT required (the grant widens the session's
            // scope to the delegated worldline, but never adds bits beyond
            // the token). The session's root grant stays; the active caps
            // become the intersection, scoped to the token's worldline.
            const LoanBody loan = loan_from();
            Capability effective{};
            if (!caps_table_.effective(loan.channel, &effective)) {
                deny(EPERM, "grant token is missing or revoked");
                break;
            }
            if (effective.workspace != workspace_id_) {
                deny(EPERM, "grant token is for another workspace");
                break;
            }
            // The token delegates keep+rollback scoped to one worldline.
            // The session KEEPS its existing read/publish bits and GAINS
            // the delegated keep+rollback, scoped to the token's worldline.
            // The grant tree already enforced attenuation at derive time,
            // so the delegated bits are a subset of the host's authority.
            Capability next = caps;
            next.bits |= effective.bits;
            next.channels |= effective.channels;
            next.worldline = effective.worldline;
            next.workspace = workspace_id_;
            if (effective.expires_gen != 0)
                next.expires_gen = effective.expires_gen;
            caps = next;
            record_audit(AuditKind::Grant, root_token, effective.worldline, loan.channel);
            // #5393: a session that activates a grant scoped to the
            // round's candidate is the round's judge.
            if (round_.active() && effective.worldline == round_.worldline())
                round_note_judge(session);
            LoanBody ok{};
            ok.channel = caps.bits;
            ok.worldline = caps.worldline;
            ok.generation = generation_;
            reply_loan(ok);
            break;
        }
        case FrameType::Revoke: {
            const LoanBody loan = loan_from();
            if (!require(kCapGrant, 0, 0))
                break;
            const std::size_t revoked = caps_table_.revoke(loan.channel);
            if (revoked == 0) {
                deny(ENOENT, "unknown grant token");
                break;
            }
            record_audit(AuditKind::Revoke, root_token, 0, revoked);
            reply_loan(loan);
            break;
        }
        case FrameType::ConstraintRollback: {
            // #5396: the constraint-change quota. A session that
            // rewrites the gate in a loop is throttled.
            if (!allow_constraint_change(session, root_token)) {
                deny(EBUSY, "rate-limited: the constraint change quota is exhausted");
                break;
            }
            if (!require(kCapConstraintWrite, 0, 0))
                break;
            std::string why;
            if (!constraints_.rollback(caps, &audit_, root_token, &why)) {
                deny(EPERM, why);
                break;
            }
            write_constraints_image();
            const LoanBody empty{};
            reply_loan(empty);
            break;
        }
        case FrameType::ConstraintInstall: {
            if (!require(kCapConstraintWrite, 0, 0))
                break;
            // #5396: the constraint-change quota (the rollback path
            // has the same guard).
            if (!allow_constraint_change(session, root_token)) {
                deny(EBUSY, "rate-limited: the constraint change quota is exhausted");
                break;
            }
            ConstraintWire wire{};
            if (header.len < sizeof(wire)) {
                deny(EINVAL, "constraint install body is too short");
                break;
            }
            std::memcpy(&wire, body.data(), sizeof(wire));
            const std::uint64_t payload = header.len - sizeof(wire);
            if (wire.name_len > payload || wire.source_len > payload - wire.name_len) {
                deny(EINVAL, "constraint install lengths are inconsistent");
                break;
            }
            const char* text = reinterpret_cast<const char*>(body.data() + sizeof(wire));
            std::string name(text, wire.name_len);
            std::string source(text + wire.name_len, wire.source_len);
            const auto pred = predicate_registry_.find(name);
            if (pred == predicate_registry_.end()) {
                deny(EPERM, "constraint source is not registered: " + name);
                break;
            }
            ConstraintSet next = constraints_.current();
            bool replaced = false;
            for (auto& rule : next.rules) {
                if (rule.name == name) {
                    rule.check = pred->second;
                    rule.source = source;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                next.rules.push_back({name, pred->second, source});
            std::string why;
            if (!constraints_.install(std::move(next), caps, &audit_, root_token, &why)) {
                deny(EPERM, why);
                break;
            }
            write_constraints_image();
            LoanBody installed{};
            installed.generation = constraints_.version();
            reply_loan(installed);
            break;
        }
        case FrameType::Disconnect:
            return false;
        default:
            break;
    }
    return true;
}

bool Server::handle_frame(PeerSession& peer, const FrameHeader& header,
                          std::span<const std::byte> body) {
    // #5396: the control-frame rate quota. A session that floods the
    // coordinator is dropped; the other sessions keep running.
    if (!allow_frame(peer)) {
        audit_.add_constraint(AuditKind::RateLimited, peer.root_token, 0, 0, peer_quota_.frame_rate,
                              "rate-limited: the session exceeded its frame quota");
        return false;
    }
    // #5395: the cost of serving a control frame is the bus's own
    // overhead — CPU time, never the wall clock (#5324).
    const std::uint64_t started_ns = slo_thread_cpu_ns();
    const bool keep = dispatch_frame(peer.conn_fd, peer.session, peer.root_token, peer.caps,
                                     peer.loans, peer.channel_loans, peer.staging_capacity,
                                     peer.staging_used, &peer.subscribed, header, body);
    const std::uint64_t cost_ns = slo_thread_cpu_ns() - started_ns;
    ++slo_.frames;
    slo_.frame_cpu_ns += cost_ns;
    slo_hist_.add(cost_ns);
    return keep;
}

void Server::finish_session(PeerSession& peer) {
    // #5396: the per-session counters die with the session.
    constraint_windows_.erase(peer.session);
    for (std::uint64_t id : worldlines_.end_session(peer.root_token)) {
        record_audit(AuditKind::Rollback, peer.root_token, id);
        project_worldline(id, WorldlineState::Rolled, ConstraintResult::None);
    }
    // #5393: a round whose participant died fails closed — the
    // pending candidate rolls back and the round is abandoned.
    if (round_.active() &&
        (peer.session == round_.owner_session() || peer.session == round_.judge_session())) {
        round_roll(peer.session == round_.owner_session() ? "proposer gone" : "judge gone");
    }
    record_audit(AuditKind::Disconnect, peer.root_token);
    peer.loans.reclaim();
    for (auto& ledger : peer.channel_loans)
        ledger.reclaim();
    peer.staging_used = 0;
    if (peer.conn_fd >= 0)
        ::close(peer.conn_fd);
    peer.conn_fd = -1;
    if (peer.event_fd >= 0)
        ::close(peer.event_fd);
    peer.event_fd = -1;
}

bool Server::wait_disconnect(TransportError* error) {
    FrameHeader header{};
    std::vector<std::byte> body(kBusMaxControlFrame);
    const auto finish = [&]() {
        for (std::uint64_t id : worldlines_.end_session(root_token_)) {
            record_audit(AuditKind::Rollback, root_token_, id);
            project_worldline(id, WorldlineState::Rolled, ConstraintResult::None);
        }
        record_audit(AuditKind::Disconnect, root_token_);
        loans_.reclaim();
        for (auto& ledger : channel_loans_)
            ledger.reclaim();
        staging_used_ = 0;
        ::close(conn_fd_);
        conn_fd_ = -1;
        return true;
    };
    while (true) {
        if (!recv_frame(conn_fd_, &header, body, error))
            return finish();
        if (!dispatch_frame(conn_fd_, session_, root_token_, session_caps_, loans_, channel_loans_,
                            staging_capacity_, staging_used_, nullptr, header,
                            {body.data(), header.len}))
            return finish();
    }
}

// ── Multi-peer coordinator (#5377) ───────────────────────────
// One poll loop over the listener, the wake eventfd and every session
// socket. All shared state (image commits, grant tree, worldline
// ledger, constraints, audit) is touched only here, so generation
// advances stay serialized without locks. A session's crash/EOF
// reclaims only its own loans and pending trials.
void Server::serve(const BuildFn& build) {
    if (listen_fd_ < 0)
        return;
    wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    // Drain one complete frame from a session's buffer; false ends it.
    const auto drain = [&](PeerSession& peer) {
        while (true) {
            const std::size_t avail = peer.inbuf.size();
            if (avail < sizeof(FrameHeader))
                return true;
            FrameHeader header{};
            std::memcpy(&header, peer.inbuf.data(), sizeof(header));
            if (header.len > kBusMaxControlFrame)
                return false;
            if (avail < sizeof(header) + header.len)
                return true;
            const std::span<const std::byte> body(peer.inbuf.data() + sizeof(header), header.len);
            const bool keep = handle_frame(peer, header, body);
            peer.inbuf.erase(peer.inbuf.begin(),
                             peer.inbuf.begin() +
                                 static_cast<std::ptrdiff_t>(sizeof(header) + header.len));
            if (!keep)
                return false;
        }
    };
    // Feed readable bytes into a session; false on EOF/error/protocol.
    const auto feed = [&](PeerSession& peer) {
        std::byte buf[4096];
        while (true) {
            std::vector<int> fds;
            const ssize_t n = recv_msg(peer.conn_fd, buf, sizeof(buf), &fds);
            for (const int f : fds)
                ::close(f); // ancillary data after Hello is unexpected
            if (n == 0) {
                return false;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return true;
                return false;
            }
            if (!peer.hello_done) {
                peer.hello_buf.insert(peer.hello_buf.end(), buf, buf + n);
                constexpr std::size_t kHelloBytes = sizeof(FrameHeader) + sizeof(HelloBody);
                if (peer.hello_buf.size() < kHelloBytes)
                    continue;
                FrameHeader header{};
                std::memcpy(&header, peer.hello_buf.data(), sizeof(header));
                if (header.type != static_cast<std::uint16_t>(FrameType::Hello) ||
                    header.len != sizeof(HelloBody)) {
                    set_reject(peer.conn_fd, 0, RejectReason::Protocol, "expected Hello");
                    return false;
                }
                HelloBody hello{};
                std::memcpy(&hello, peer.hello_buf.data() + sizeof(header), sizeof(hello));
                TransportError err;
                if (!handshake(peer.conn_fd, hello, build, &peer.session, &peer.caps,
                               &peer.root_token, &peer.event_fd, &peer.staging_capacity, nullptr,
                               &err)) {
                    return false;
                }
                if (peer.caps.max_loans > 0) {
                    const auto cap = static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(peer.caps.max_loans, kBusMaxLoans));
                    for (auto& ledger : peer.channel_loans)
                        ledger.set_cap(cap);
                }
                peer.hello_done = true;
                peer.inbuf.assign(peer.hello_buf.begin() + static_cast<std::ptrdiff_t>(kHelloBytes),
                                  peer.hello_buf.end());
                peer.hello_buf.clear();
                peer.hello_buf.shrink_to_fit();
                if (!drain(peer))
                    return false;
                continue;
            }
            peer.inbuf.insert(peer.inbuf.end(), buf, buf + n);
            if (!drain(peer))
                return false;
        }
    };
    while (true) {
        std::vector<pollfd> pfds;
        std::vector<std::size_t> peer_at; // pfds index -> peers_ index
        pfds.push_back({listen_fd_, POLLIN, 0});
        if (wake_fd_ >= 0)
            pfds.push_back({wake_fd_, POLLIN, 0});
        for (std::size_t i = 0; i < peers_.size(); ++i) {
            if (peers_[i].conn_fd < 0)
                continue;
            peer_at.push_back(i);
            pfds.push_back({peers_[i].conn_fd, POLLIN, 0});
        }
        std::size_t polled_peers = peer_at.size();
        // #5393: a running round wakes the loop now and then, so the
        // wall budget closes it even when nothing happens.
        int timeout = round_.active() ? 100 : -1;
        // #5396: a peer that is still waiting for its hello needs a
        // wakeup when its handshake deadline passes.
        if (timeout < 0) {
            std::uint64_t earliest = 0;
            for (const auto& peer : peers_) {
                if (peer.hello_done || peer.hello_deadline_ms == 0)
                    continue;
                if (earliest == 0 || peer.hello_deadline_ms < earliest)
                    earliest = peer.hello_deadline_ms;
            }
            if (earliest != 0) {
                const std::uint64_t now = round_now_ms();
                const std::uint64_t delta = earliest > now ? earliest - now : 0;
                timeout = static_cast<int>(std::min<std::uint64_t>(delta + 1, 1000));
            }
        }
        const int ready = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        // #5393: the round budget is checked on every wakeup, so a
        // round that runs out of time is closed even when the peers go
        // quiet.
        round_budget_check();
        // The wake bell is a wakeup, not an exit: only a stop() request
        // stops the coordinator. A stray write (or a recycled
        // descriptor) must not take the bus down.
        if (wake_fd_ >= 0 && (pfds[1].revents & POLLIN) != 0) {
            std::uint64_t count = 0;
            ssize_t ignored = ::read(wake_fd_, &count, sizeof(count));
            (void)ignored;
            if (stop_requested_)
                break;
        }
        if ((pfds[0].revents & POLLIN) != 0) {
            // Accept every pending connection without blocking: the
            // listener is nonblocking only for this drain, then restored
            // so stop()'s shutdown still wakes a blocked poll/accept.
            const int lfl = ::fcntl(listen_fd_, F_GETFL, 0);
            ::fcntl(listen_fd_, F_SETFL, lfl | O_NONBLOCK);
            while (true) {
                const int fd =
                    ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (fd < 0) {
                    break;
                }
                PeerSession peer;
                peer.conn_fd = fd;
                // #5396: a connection that never says hello must not
                // hold a session slot forever.
                peer.hello_deadline_ms = round_now_ms() + peer_quota_.hello_timeout_ms;
                peer.frame_refill_ms = round_now_ms();
                peer.frame_tokens = peer_quota_.frame_burst * 1000;
                peers_.push_back(std::move(peer));
                // Feed the new peer immediately: its Hello may already be
                // readable and the client blocks on HelloOk.
                if (!feed(peers_.back())) {
                    finish_session(peers_.back());
                    peers_.pop_back();
                }
            }
            ::fcntl(listen_fd_, F_SETFL, lfl);
        }
        if ((pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            break; // listener shut down by stop()
        }
        // Service the peers present at poll time. A feed may dispatch a
        // frame whose handler (KEEP apply) re-enters the bus from the host
        // eval thread; that is serialized by g_bus_mutex on the eval side.
        // Newly accepted peers are fed on the next iteration.
        std::vector<std::size_t> ended;
        for (std::size_t p = 0; p < polled_peers; ++p) {
            PeerSession& peer = peers_[peer_at[p]];
            const short rev = pfds[2 + p].revents;
            if ((rev & (POLLERR | POLLNVAL)) != 0)
                ended.push_back(peer_at[p]);
            else if ((rev & (POLLIN | POLLHUP)) != 0 && !feed(peer))
                ended.push_back(peer_at[p]);
        }
        // Feed newly accepted peers that have not completed hello yet.
        for (std::size_t i = polled_peers; i < peers_.size(); ++i) {
            if (peers_[i].conn_fd >= 0 && !peers_[i].hello_done) {
                if (!feed(peers_[i]))
                    ended.push_back(i);
            }
        }
        // #5396: the handshake deadline. A peer that connected and
        // never said hello is dropped; the others are untouched.
        {
            const std::uint64_t now_ms = round_now_ms();
            for (std::size_t i = 0; i < peers_.size(); ++i) {
                if (peers_[i].hello_done || peers_[i].hello_deadline_ms == 0)
                    continue;
                if (now_ms < peers_[i].hello_deadline_ms)
                    continue;
                audit_.add_constraint(AuditKind::RateLimited, peers_[i].root_token, 0, 0,
                                      peer_quota_.hello_timeout_ms,
                                      "rate-limited: the handshake deadline passed");
                ended.push_back(i);
            }
        }
        // Erase high indices first so the lower ones stay valid. A peer
        // may be listed twice (its feed failed and its handshake
        // deadline passed) — it is finished once.
        std::sort(ended.begin(), ended.end(), std::greater<std::size_t>());
        ended.erase(std::unique(ended.begin(), ended.end()), ended.end());
        for (const std::size_t i : ended) {
            finish_session(peers_[i]);
            peers_.erase(peers_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    for (auto& peer : peers_)
        finish_session(peer);
    peers_.clear();
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
}

bool Client::connect_to(const std::string& path, std::uint64_t workspace_id, std::uint32_t role,
                        std::uint64_t caps, Attach* out, TransportError* error) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error)
            *error = {errno, "socket failed"};
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        if (error)
            *error = {ENAMETOOLONG, "socket path too long"};
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (error)
            *error = {errno, "connect failed"};
        ::close(fd);
        return false;
    }
    HelloBody hello{};
    hello.abi_major = static_cast<std::uint16_t>(kBusAbiMajor);
    hello.abi_minor = static_cast<std::uint16_t>(kBusAbiMinor);
    const SchemaId id = header_schema_id();
    std::memcpy(hello.schema_id, &id.a, 8);
    std::memcpy(hello.schema_id + 8, &id.b, 8);
    hello.layout_version = 1;
    hello.pid = static_cast<std::uint32_t>(::getpid());
    hello.role = role;
    hello.caps = caps & kCapAll;
    hello.channels = ~0ull;
    hello.max_loans = 0;
    hello.workspace_id = workspace_id;
    hello.image_bytes = 0;
    if (!send_frame(fd, FrameType::Hello, 0,
                    {reinterpret_cast<const std::byte*>(&hello), sizeof(hello)})) {
        if (error)
            *error = {errno, "send Hello failed"};
        ::close(fd);
        return false;
    }

    FrameHeader header{};
    int fds[4] = {-1, -1, -1, -1};
    std::size_t nfds = 0;
    if (!recv_header(fd, &header, fds, 4, &nfds)) {
        if (error)
            *error = {errno ? errno : EPIPE, "no Hello reply"};
        ::close(fd);
        return false;
    }
    std::vector<std::byte> body(header.len <= kBusMaxControlFrame ? header.len : 0);
    if (header.len > kBusMaxControlFrame ||
        (header.len && !read_all(fd, body.data(), header.len))) {
        for (std::size_t i = 0; i < nfds; ++i)
            ::close(fds[i]);
        ::close(fd);
        if (error)
            *error = {EMSGSIZE, "bad Hello reply"};
        return false;
    }
    if (header.type == static_cast<std::uint16_t>(FrameType::HelloReject)) {
        HelloRejectBody rej{};
        if (body.size() >= sizeof(rej))
            std::memcpy(&rej, body.data(), sizeof(rej));
        for (std::size_t i = 0; i < nfds; ++i)
            ::close(fds[i]);
        ::close(fd);
        if (error)
            *error = {rej.reason, std::string("attach rejected: ") + std::string(rej.detail)};
        return false;
    }
    if (header.type != static_cast<std::uint16_t>(FrameType::HelloOk) ||
        body.size() != sizeof(HelloOkBody) || nfds != 2) {
        for (std::size_t i = 0; i < nfds; ++i)
            ::close(fds[i]);
        ::close(fd);
        if (error)
            *error = {EPROTO, "malformed HelloOk"};
        return false;
    }
    HelloOkBody ok{};
    std::memcpy(&ok, body.data(), sizeof(ok));
    const int image_fd = fds[0];
    const int event_fd = fds[1];
    if (!has_required_seals(image_fd)) {
        ::close(image_fd);
        ::close(event_fd);
        ::close(fd);
        if (error)
            *error = {static_cast<int>(RejectReason::Seals), "image lacks shrink/grow seals"};
        return false;
    }
    const int prot =
        (ok.caps & (kCapPublish | kCapTrial)) != 0 ? (PROT_READ | PROT_WRITE) : PROT_READ;
    void* base = ::mmap(nullptr, ok.image_size, prot, MAP_SHARED, image_fd, 0);
    if (base == MAP_FAILED) {
        ::close(image_fd);
        ::close(event_fd);
        ::close(fd);
        if (error)
            *error = {errno, "mmap failed"};
        return false;
    }
    const auto view = std::span<const std::byte>(static_cast<const std::byte*>(base),
                                                 static_cast<std::size_t>(ok.image_size));
    const ImageCheck check = validate_image(view);
    if (!check.ok()) {
        ::munmap(base, ok.image_size);
        ::close(image_fd);
        ::close(event_fd);
        ::close(fd);
        if (error)
            *error = {EINVAL, std::string("image rejected: ") + check.detail};
        return false;
    }
    out->sock = fd;
    out->event = event_fd;
    out->image_fd = image_fd;
    out->base = base;
    out->size = static_cast<std::size_t>(ok.image_size);
    out->session = ok.session;
    out->caps = ok.caps;
    return true;
}

} // namespace aura::bus
