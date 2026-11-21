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
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
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

    bool recv_header(int fd, FrameHeader* header, int* fds, std::size_t max_fds,
                     std::size_t* nfds) {
        struct iovec iov{};
        iov.iov_base = header;
        iov.iov_len = sizeof(FrameHeader);
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
            return false;
        *nfds = 0;
        for (auto* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
                continue;
            const std::size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (std::size_t i = 0; i < count && *nfds < max_fds; ++i) {
                int received = -1;
                std::memcpy(&received, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
                fds[(*nfds)++] = received;
            }
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
    if (conn_fd_ >= 0)
        ::close(conn_fd_);
    if (listen_fd_ >= 0)
        ::close(listen_fd_);
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
    // Shutdown, not close: the serving thread may be blocked in accept
    // or recv on these fds and must observe the error before closing.
    if (listen_fd_ >= 0)
        ::shutdown(listen_fd_, SHUT_RDWR);
    if (conn_fd_ >= 0)
        ::shutdown(conn_fd_, SHUT_RDWR);
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
    if (::listen(listen_fd_, 1) != 0) {
        if (error)
            *error = {errno, "listen failed"};
        return false;
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
    ucred cred{};
    socklen_t cred_len = sizeof(cred);
    if (getsockopt(conn_fd_, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 ||
        cred.uid != ::getuid()) {
        set_reject(conn_fd_, 0, RejectReason::Permission, "peer uid differs");
        if (error)
            *error = {EPERM, "peer uid differs"};
        return false;
    }

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
    const SchemaId header_id = header_schema_id();
    std::uint8_t expected[16] = {};
    std::memcpy(expected, &header_id.a, 8);
    std::memcpy(expected + 8, &header_id.b, 8);
    const auto reject = [&](RejectReason reason, std::string_view why, int code) {
        set_reject(conn_fd_, 0, reason, why);
        if (error)
            *error = {code, std::string(why)};
        return false;
    };
    if (hello.abi_major != kBusAbiMajor)
        return reject(RejectReason::Version, "ABI major mismatch", EPROTO);
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
    session_caps_ = granted;
    if (granted.max_loans > 0) {
        const auto cap =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(granted.max_loans, kBusMaxLoans));
        for (auto& ledger : channel_loans_)
            ledger.set_cap(cap);
    }
    root_token_ = caps_table_.issue_root(granted);

    // Issue #5128: one bus = one workspace = one live image. The image
    // outlives peer sessions: the first attach creates and builds it,
    // every later peer is handed the SAME memfd, so the next tool reads
    // the previous tool's committed records instead of a fresh layout.
    // Disconnect withdraws trials and loans, never committed evidence.
    std::size_t bytes = image_size_;
    int image_fd = -1;
    // The peer's mapping: fresh on the first attach, a new mapping of the
    // persistent image on every later one. The host's own mapping
    // (image_base_) is never handed to an Attach — that would unmap it
    // when the peer detaches.
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

    session_ = (static_cast<std::uint64_t>(::getpid()) << 32) ^ ++g_session_counter;
    HelloOkBody ok{};
    ok.session = session_;
    ok.caps = granted.bits;
    ok.channels = granted.channels;
    ok.max_loans = granted.max_loans;
    ok.image_size = bytes;
    std::memcpy(ok.schema_id, expected, sizeof(expected));
    ok.committed_bytes = ih.committed_bytes;
    FrameHeader ok_header{};
    ok_header.len = sizeof(ok);
    ok_header.type = static_cast<std::uint16_t>(FrameType::HelloOk);
    ok_header.session = session_;
    std::vector<std::byte> packet(sizeof(ok_header) + sizeof(ok));
    std::memcpy(packet.data(), &ok_header, sizeof(ok_header));
    std::memcpy(packet.data() + sizeof(ok_header), &ok, sizeof(ok));
    const int fds[2] = {image_fd, event_fd};
    if (!send_with_fds(conn_fd_, packet.data(), packet.size(), fds, 2)) {
        ::munmap(base, bytes);
        ::close(image_fd);
        ::close(event_fd);
        if (error)
            *error = {errno, "sendmsg HelloOk failed"};
        return false;
    }

    out->event = event_fd;
    out->image_fd = image_fd;
    out->base = base;
    out->size = bytes;
    out->session = session_;
    out->caps = ok.caps;
    // Reserve grants slices of a fixed staging window; exhaustion is a
    // hard error, never a resize.
    staging_capacity_ = std::min<std::uint64_t>(
        64u << 10, ih.committed_bytes < bytes ? bytes - ih.committed_bytes : 0);
    staging_used_ = 0;
    if (first_image) {
        image_base_ = static_cast<std::byte*>(base);
        image_size_ = bytes;
        host_image_fd_ = ::dup(image_fd);
        // The host's mapping must outlive the peer's Attach: give the
        // peer its own mapping of the same memfd.
        void* peer_base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, image_fd, 0);
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
        out->base = peer_base;
    }
    write_constraints_image();
    record_audit(AuditKind::Attach, root_token_, 0, hello.pid);
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

bool Server::wait_disconnect(TransportError* error) {
    FrameHeader header{};
    std::vector<std::byte> body(kBusMaxControlFrame);
    // Issue #5128: the coordinator projects its trial ledger into the
    // worldlines channel so subscribers (harness) see every trial and its
    // verdict, not just a producer's own workspace projections. Records
    // are appended in id order; a settled trial appends its final state.
    std::uint64_t& projected_trials = projected_trials_;
    const auto project_worldline = [&](std::uint64_t id, WorldlineState state,
                                       ConstraintResult cr) {
        if (image_base_ == nullptr)
            return;
        SectionEntry se{};
        if (!find_section({image_base_, image_size_}, SectionKind::Worldlines, &se))
            return;
        ChannelView view{};
        if (!channel_view({image_base_, image_size_}, Channel::Worldlines, &view))
            return;
        if (view.length + sizeof(WorldlineRecord) > se.capacity)
            return; // capacity exhaustion stays a hard error on the publish
                    // path; the control-plane ledger remains authoritative
        WorldlineRecord w{};
        w.id = id;
        w.parent = kImageNoParent;
        w.state = static_cast<std::uint8_t>(state);
        w.constraint_result = static_cast<std::uint8_t>(cr);
        w.metrics_off = kImageNoIndex;
        std::memcpy(image_base_ + view.offset + view.length, &w, sizeof(w));
        std::uint64_t gen = 0;
        std::string why;
        if (channel_commit({image_base_, image_size_}, Channel::Worldlines,
                           view.offset + view.length, sizeof(w), &gen, &why)) {
            projected_trials = id;
            generation_ = gen;
        }
    };
    const auto deny = [&](int code, const std::string& why) {
        ErrorBody eb{};
        eb.code = static_cast<std::uint32_t>(code);
        const std::size_t n = std::min(why.size(), sizeof(eb.message) - 1);
        std::memcpy(eb.message, why.data(), n);
        send_frame(conn_fd_, FrameType::Error, session_,
                   {reinterpret_cast<const std::byte*>(&eb), sizeof(eb)});
        record_audit(AuditKind::Denied, root_token_);
    };
    const auto require = [&](std::uint64_t bit, std::uint64_t channel, std::uint64_t worldline) {
        std::string why;
        if (allows(session_caps_, bit, channel, generation_, worldline, &why))
            return true;
        deny(EPERM, why);
        return false;
    };
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

        const auto loan_from = [&]() {
            LoanBody loan{};
            if (header.len >= sizeof(loan))
                std::memcpy(&loan, body.data(), sizeof(loan));
            return loan;
        };
        const auto reply_loan = [&](const LoanBody& loan) {
            send_frame(conn_fd_, FrameType::Credit, session_,
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
                                      view.offset,  view.length,     1};
                    send_frame(conn_fd_, FrameType::Credit, session_,
                               {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
                    if (header.type == static_cast<std::uint16_t>(FrameType::Subscribe))
                        record_audit(AuditKind::Subscribe, root_token_, 0, loan.channel);
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
                send_frame(conn_fd_, FrameType::Credit, session_, out);
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
                const bool snapshot = request.generation == 0 ||
                                      request.generation > view.generation ||
                                      request.count > view.count;
                const std::uint64_t from = snapshot ? 0 : request.count;
                ChannelBody reply{request.channel,
                                  view.generation,
                                  view.count,
                                  view.offset + from * view.element_size,
                                  (view.count - from) * view.element_size,
                                  static_cast<std::uint64_t>(snapshot ? 1 : 0)};
                send_frame(conn_fd_, FrameType::Credit, session_,
                           {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
                break;
            }
            case FrameType::Reserve: {
                const LoanBody loan = loan_from();
                Channel channel{};
                if (channel_from(loan.channel, &channel) && image_base_ != nullptr) {
                    if (!require(kCapPublish, loan.channel, 0))
                        break;
                    auto& ledger = channel_loans_[static_cast<std::size_t>(channel)];
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
                if (!loans_.reserve()) {
                    deny(EAGAIN, "loan quota exhausted");
                    break;
                }
                if (loan.length == 0 || loan.length > staging_capacity_ - staging_used_) {
                    loans_.release();
                    deny(ENOSPC, "staging window exhausted");
                    break;
                }
                LoanBody granted = loan;
                granted.offset = staging_used_;
                granted.generation = generation_;
                staging_used_ += loan.length;
                reply_loan(granted);
                break;
            }
            case FrameType::Publish: {
                const LoanBody loan = loan_from();
                Channel channel{};
                if (channel_from(loan.channel, &channel) && image_base_ != nullptr) {
                    if (!require(kCapPublish, loan.channel, loan.worldline))
                        break;
                    std::uint64_t gen = 0;
                    std::string why;
                    if (!channel_commit({image_base_, image_size_}, channel, loan.offset,
                                        loan.length, &gen, &why)) {
                        deny(EINVAL, why);
                        break;
                    }
                    channel_loans_[static_cast<std::size_t>(channel)].release();
                    ChannelView view{};
                    channel_view({image_base_, image_size_}, channel, &view);
                    ChannelBody reply{loan.channel, gen, view.count, view.offset, view.length, 0};
                    send_frame(conn_fd_, FrameType::Credit, session_,
                               {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
                    generation_ = gen;
                    record_audit(AuditKind::Publish, root_token_, 0, loan.length);
                    break;
                }
                if (!require(kCapPublish, loan.channel, loan.worldline))
                    break;
                if (loan.length == 0 || loan.offset > staging_capacity_ ||
                    loan.length > staging_capacity_ - loan.offset) {
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
                    channel_loans_[static_cast<std::size_t>(channel)].release();
                else
                    loans_.release();
                break;
            }
            case FrameType::Trial: {
                const LoanBody loan = loan_from();
                if (!require(kCapTrial, loan.channel, 0))
                    break;
                const std::uint64_t id = worldlines_.open_trial(root_token_, workspace_id_);
                record_audit(AuditKind::Trial, root_token_, id);
                project_worldline(id, WorldlineState::Pending, ConstraintResult::None);
                LoanBody granted = loan;
                granted.worldline = id;
                granted.generation = generation_;
                reply_loan(granted);
                break;
            }
            case FrameType::Keep:
            case FrameType::Rollback: {
                const LoanBody loan = loan_from();
                const bool keep = header.type == static_cast<std::uint16_t>(FrameType::Keep);
                if (!require(keep ? kCapKeep : kCapRollback, loan.channel, loan.worldline))
                    break;
                if (keep) {
                    const GateResult gate = constraints_.check(
                        {loan.worldline, loan.offset, loan.length}, &audit_, root_token_);
                    if (!gate.pass) {
                        deny(EPERM, "constraint: " + gate.reason);
                        break;
                    }
                }
                std::string why;
                const bool ok =
                    keep ? worldlines_.keep(loan.worldline, root_token_, session_caps_, &why)
                         : worldlines_.rollback(loan.worldline, root_token_, session_caps_, &why);
                if (!ok) {
                    deny(EPERM, why);
                    break;
                }
                record_audit(keep ? AuditKind::Keep : AuditKind::Rollback, root_token_,
                             loan.worldline);
                project_worldline(loan.worldline,
                                  keep ? WorldlineState::Kept : WorldlineState::Rolled,
                                  keep ? ConstraintResult::Pass : ConstraintResult::None);
                reply_loan(loan);
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
                const std::uint64_t parent = request.parent == 0 ? root_token_ : request.parent;
                std::string why;
                const auto token = caps_table_.derive(parent, request.caps, &why);
                if (!token) {
                    deny(EPERM, why);
                    break;
                }
                GrantReply reply{};
                reply.token = *token;
                send_frame(conn_fd_, FrameType::Credit, session_,
                           {reinterpret_cast<const std::byte*>(&reply), sizeof(reply)});
                record_audit(AuditKind::Grant, root_token_, 0, *token);
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
                record_audit(AuditKind::Revoke, root_token_, 0, revoked);
                reply_loan(loan);
                break;
            }
            case FrameType::ConstraintRollback: {
                if (!require(kCapConstraintWrite, 0, 0))
                    break;
                std::string why;
                if (!constraints_.rollback(session_caps_, &audit_, root_token_, &why)) {
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
                if (!constraints_.install(std::move(next), session_caps_, &audit_, root_token_,
                                          &why)) {
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
                return finish();
            default:
                break;
        }
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
