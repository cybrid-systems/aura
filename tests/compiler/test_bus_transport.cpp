// Issue #5123: local bus transport — handshake, SCM_RIGHTS image, seals,
// loan cap and peer-death reclaim. No capabilities or channel semantics.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus/image_schema.hh"
#include "bus/transport.hh"

namespace {

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

using namespace aura::bus;

bool build_empty_image(std::span<std::byte> img, std::string*) {
    if (img.size() < kImageHeaderSize)
        return false;
    std::memset(img.data(), 0, img.size());
    ImageHeader h{};
    std::memcpy(h.magic, kImageMagic, sizeof(h.magic));
    h.abi_major = kImageAbiMajor;
    h.abi_minor = kImageAbiMinor;
    h.endianness = static_cast<std::uint32_t>(kImageEndianMarker);
    h.header_len = kImageHeaderSize;
    h.image_size = img.size();
    const SchemaId id = header_schema_id();
    std::memcpy(h.schema_id, &id.a, 8);
    std::memcpy(h.schema_id + 8, &id.b, 8);
    h.layout_version = 1;
    h.committed_bytes = kImageHeaderSize;
    h.committed_gen = 2;
    h.section_dir_off = kImageHeaderSize;
    h.section_count = 0;
    std::memcpy(img.data(), &h, sizeof(h));
    return true;
}

std::string test_path(const char* tag) {
    return "/tmp/aura-bus-" + std::to_string(::getpid()) + "-" + tag + ".sock";
}

bool send_loan(int fd, FrameType type, std::uint64_t session) {
    LoanBody body{};
    body.channel = 1;
    body.offset = 0;
    body.length = 64;
    body.generation = 2;
    return send_frame(fd, type, session, {reinterpret_cast<const std::byte*>(&body), sizeof(body)});
}

// Server child: handshake, then wait for EOF and report reclaimed loans.
[[noreturn]] void server_main(const std::string& path, std::uint64_t workspace, int ready_fd,
                              int result_fd) {
    Server server;
    TransportError err;
    if (!server.listen_at(path, 1u << 16, workspace, &err))
        _exit(20);
    WorkspacePolicy policy;
    policy.allow(/*role=*/0, Capability{kCapAll, 0xFF, workspace, 0, 0, 0});
    server.set_policy(policy);
    const char ready = 'R';
    if (::write(ready_fd, &ready, 1) != 1)
        _exit(21);
    Attach peer;
    if (!server.accept_one(build_empty_image, &peer, &err))
        _exit(22);
    if (!server.wait_disconnect(&err))
        _exit(23);
    const std::uint64_t reclaimed = server.loans().reclaimed_total();
    const std::uint64_t outstanding = server.loans().outstanding();
    std::uint64_t out[5] = {reclaimed, outstanding, server.audit().count(AuditKind::Rollback),
                            server.audit().count(AuditKind::Disconnect),
                            server.audit().count(AuditKind::Keep)};
    if (::write(result_fd, out, sizeof(out)) != static_cast<ssize_t>(sizeof(out)))
        _exit(24);
    _exit(0);
}

} // namespace

int main() {
    // ── Seal and ledger units ────────────────────────────────────
    {
        TransportError err;
        const int sealed = create_sealed_image(8192, &err);
        check(sealed >= 0, "create_sealed_image");
        if (sealed >= 0) {
            check(has_required_seals(sealed), "shrink/grow seals present");
            const int seals = fcntl(sealed, F_GET_SEALS);
            check((seals & F_SEAL_SHRINK) != 0, "F_SEAL_SHRINK");
            check((seals & F_SEAL_GROW) != 0, "F_SEAL_GROW");
            check((seals & F_SEAL_WRITE) == 0, "no F_SEAL_WRITE on a mapped image");
            ::close(sealed);
        }
        const int plain = static_cast<int>(syscall(SYS_memfd_create, "plain", 0));
        check(plain >= 0, "plain memfd");
        if (plain >= 0) {
            check(!has_required_seals(plain), "unsealed fd is rejected");
            ::close(plain);
        }
    }
    {
        LoanLedger ledger(2);
        check(ledger.reserve(), "loan 1");
        check(ledger.reserve(), "loan 2");
        check(!ledger.reserve(), "loan cap is a hard error");
        ledger.release();
        check(ledger.reserve(), "release frees a loan");
        check(ledger.reclaim() == 2, "reclaim counts outstanding");
        check(ledger.outstanding() == 0, "reclaim clears outstanding");
    }

    // ── End-to-end attach, loan cap, peer death ──────────────────
    {
        const std::string path = test_path("main");
        int ready[2];
        int result[2];
        check(pipe(ready) == 0 && pipe(result) == 0, "pipes");
        const pid_t pid = fork();
        check(pid >= 0, "fork server");
        if (pid == 0) {
            ::close(ready[0]);
            ::close(result[0]);
            server_main(path, 42, ready[1], result[1]);
        }
        ::close(ready[1]);
        ::close(result[1]);
        char ready_byte = 0;
        check(::read(ready[0], &ready_byte, 1) == 1 && ready_byte == 'R', "server ready");

        Attach att;
        TransportError err;
        Client client;
        const bool attached = client.connect_to(path, 42, /*role=*/0, kCapAll, &att, &err);
        check(attached, "client attaches");
        if (attached) {
            check(att.session != 0, "session id issued");
            check(att.caps == kCapAll, "requested capabilities granted");
            check(has_required_seals(att.image_fd), "peer checks seals");
            check(att.view().size() == (1u << 16), "negotiated image size");
            const auto vc = validate_image(att.view());
            check(vc.ok(), "attached image validates");
            const ImageHeader h = load_header(att.view());
            check(h.section_count == 0, "empty image has no sections");

            // Doorbell is a real eventfd.
            const std::uint64_t one = 1;
            check(::write(att.event, &one, sizeof(one)) == sizeof(one), "eventfd writable");

            // 8 reserves fit and are granted; the 9th is a hard error.
            FrameHeader reply{};
            std::vector<std::byte> body(kBusMaxControlFrame);
            std::uint64_t next_offset = 0;
            for (int i = 0; i < 8; ++i) {
                check(send_loan(att.sock, FrameType::Reserve, att.session), "send Reserve");
                check(recv_frame(att.sock, &reply, body, &err), "loan grant reply");
                check(reply.type == static_cast<std::uint16_t>(FrameType::Credit), "loan granted");
                LoanBody granted{};
                if (reply.len >= sizeof(granted)) {
                    std::memcpy(&granted, body.data(), sizeof(granted));
                    check(granted.offset == next_offset, "staging offset advances");
                    next_offset += granted.length;
                }
            }
            check(send_loan(att.sock, FrameType::Reserve, att.session), "send 9th Reserve");
            check(recv_frame(att.sock, &reply, body, &err), "loan cap reply");
            check(reply.type == static_cast<std::uint16_t>(FrameType::Error), "loan cap error");
            if (reply.len >= sizeof(std::uint32_t)) {
                std::uint32_t reason = 0;
                std::memcpy(&reason, body.data(), sizeof(reason));
                check(reason == EAGAIN, "loan cap reports EAGAIN");
            }

            // Release every loan, then ask for more than the staging
            // window: a hard ENOSPC, never a resize.
            for (int i = 0; i < 8; ++i)
                check(send_loan(att.sock, FrameType::Credit, att.session), "send Credit");
            {
                LoanBody big{};
                big.channel = 1;
                big.length = 1u << 20;
                check(send_frame(att.sock, FrameType::Reserve, att.session,
                                 {reinterpret_cast<const std::byte*>(&big), sizeof(big)}),
                      "send oversized Reserve");
                check(recv_frame(att.sock, &reply, body, &err), "ENOSPC reply");
                check(reply.type == static_cast<std::uint16_t>(FrameType::Error), "space error");
                if (reply.len >= sizeof(std::uint32_t)) {
                    std::uint32_t reason = 0;
                    std::memcpy(&reason, body.data(), sizeof(reason));
                    check(reason == ENOSPC, "staging exhaustion reports ENOSPC");
                }
            }

            // One kept trial and one pending trial: disconnect must roll
            // the pending one back and keep the audit trail.
            std::uint64_t kept_worldline = 0;
            {
                LoanBody trial{};
                trial.channel = 1;
                check(send_frame(att.sock, FrameType::Trial, att.session,
                                 {reinterpret_cast<const std::byte*>(&trial), sizeof(trial)}),
                      "open kept trial");
                check(recv_frame(att.sock, &reply, body, &err), "kept trial reply");
                check(reply.type == static_cast<std::uint16_t>(FrameType::Credit), "trial granted");
                LoanBody granted{};
                if (reply.len >= sizeof(granted)) {
                    std::memcpy(&granted, body.data(), sizeof(granted));
                    kept_worldline = granted.worldline;
                }
                check(kept_worldline != 0, "trial worldline id");
                LoanBody keep{};
                keep.channel = 1;
                keep.worldline = kept_worldline;
                check(send_frame(att.sock, FrameType::Keep, att.session,
                                 {reinterpret_cast<const std::byte*>(&keep), sizeof(keep)}),
                      "keep trial");
                check(recv_frame(att.sock, &reply, body, &err), "keep reply");
                check(reply.type == static_cast<std::uint16_t>(FrameType::Credit), "keep accepted");
            }
            {
                LoanBody trial{};
                trial.channel = 1;
                check(send_frame(att.sock, FrameType::Trial, att.session,
                                 {reinterpret_cast<const std::byte*>(&trial), sizeof(trial)}),
                      "open pending trial");
                check(recv_frame(att.sock, &reply, body, &err), "pending trial reply");
                check(reply.type == static_cast<std::uint16_t>(FrameType::Credit),
                      "pending trial granted");
            }

            // Leave three uncommitted loans for the peer-death reclaim.
            for (int i = 0; i < 3; ++i) {
                check(send_loan(att.sock, FrameType::Reserve, att.session), "send reclaim Reserve");
                check(recv_frame(att.sock, &reply, body, &err), "reclaim grant reply");
                check(reply.type == static_cast<std::uint16_t>(FrameType::Credit),
                      "reclaim loan granted");
            }

            // Peer death: close the socket abruptly, keep the mapping
            // alive until the server has observed the disconnect.
            const int sock = att.sock;
            att.sock = -1;
            ::close(sock);
            std::uint64_t out[5] = {0, 0, 0, 0, 0};
            check(::read(result[0], out, sizeof(out)) == static_cast<ssize_t>(sizeof(out)),
                  "server reports reclaim");
            check(out[0] == 3, "all three uncommitted loans reclaimed");
            check(out[1] == 0, "no outstanding loan after reclaim");
            check(out[2] == 1, "pending trial rolled back on disconnect");
            check(out[3] == 1, "disconnect audited");
            check(out[4] == 1, "keep audited");
        } else {
            std::printf("attach error: %s (%d)\n", err.message.c_str(), err.code);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "server exits cleanly");
        ::close(ready[0]);
        ::close(result[0]);
        ::unlink(path.c_str());
    }

    // ── Handshake rejection: workspace mismatch ─────────────────
    {
        const std::string path = test_path("reject");
        int ready[2];
        check(pipe(ready) == 0, "reject pipe");
        const pid_t pid = fork();
        check(pid >= 0, "fork reject server");
        if (pid == 0) {
            ::close(ready[0]);
            Server server;
            TransportError err;
            if (!server.listen_at(path, 1u << 16, 7, &err))
                _exit(30);
            const char r = 'R';
            if (::write(ready[1], &r, 1) != 1)
                _exit(32);
            Attach peer;
            // The handshake must fail; exit 0 on a clean rejection.
            const bool ok = server.accept_one(build_empty_image, &peer, &err);
            _exit(ok ? 31 : 0);
        }
        ::close(ready[1]);
        char r = 0;
        check(::read(ready[0], &r, 1) == 1, "reject server ready");
        Attach att;
        TransportError err;
        Client client;
        const bool ok = client.connect_to(path, /*workspace=*/99, 0, kCapRead, &att, &err);
        check(!ok, "workspace mismatch is rejected");
        check(err.code == static_cast<int>(RejectReason::Workspace), "reject reason workspace");
        check(err.message.find("workspace") != std::string::npos, "reject detail names workspace");
        int status = 0;
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "reject server cleaned up");
        ::close(ready[0]);
        ::unlink(path.c_str());
    }

    // ── Read-only session cannot publish, trial or keep ─────────
    {
        const std::string path = test_path("readonly");
        int ready[2];
        check(pipe(ready) == 0, "readonly pipe");
        const pid_t pid = fork();
        check(pid >= 0, "fork readonly server");
        if (pid == 0) {
            ::close(ready[0]);
            Server server;
            TransportError err;
            if (!server.listen_at(path, 1u << 16, 55, &err))
                _exit(40);
            WorkspacePolicy policy;
            policy.allow(/*role=*/1, Capability{kCapRead, 0xFF, 55, 0, 0, 0});
            server.set_policy(policy);
            const char r = 'R';
            if (::write(ready[1], &r, 1) != 1)
                _exit(41);
            Attach peer;
            if (!server.accept_one(build_empty_image, &peer, &err))
                _exit(42);
            if (!server.wait_disconnect(&err))
                _exit(43);
            _exit(server.audit().count(AuditKind::Denied) >= 3 ? 0 : 44);
        }
        ::close(ready[1]);
        char r = 0;
        check(::read(ready[0], &r, 1) == 1, "readonly server ready");
        Attach att;
        TransportError err;
        Client client;
        check(client.connect_to(path, 55, /*role=*/1, kCapAll, &att, &err), "read-only attach");
        check(att.caps == kCapRead, "policy narrows to read");
        FrameHeader reply{};
        std::vector<std::byte> body(kBusMaxControlFrame);
        check(send_loan(att.sock, FrameType::Reserve, att.session), "read-only Reserve");
        check(recv_frame(att.sock, &reply, body, &err), "read-only Reserve reply");
        check(reply.type == static_cast<std::uint16_t>(FrameType::Error), "Reserve denied");
        if (reply.len >= sizeof(ErrorBody)) {
            ErrorBody er{};
            std::memcpy(&er, body.data(), sizeof(er));
            check(er.code == EPERM, "Reserve EPERM");
            check(std::string(er.message).find("publish") != std::string::npos,
                  "Reserve names publish");
        }
        check(send_loan(att.sock, FrameType::Trial, att.session), "read-only Trial");
        check(recv_frame(att.sock, &reply, body, &err), "read-only Trial reply");
        check(reply.type == static_cast<std::uint16_t>(FrameType::Error), "Trial denied");
        if (reply.len >= sizeof(ErrorBody)) {
            ErrorBody er{};
            std::memcpy(&er, body.data(), sizeof(er));
            check(std::string(er.message).find("trial") != std::string::npos, "Trial names trial");
        }
        {
            LoanBody keep{};
            keep.channel = 1;
            keep.worldline = 1;
            check(send_frame(att.sock, FrameType::Keep, att.session,
                             {reinterpret_cast<const std::byte*>(&keep), sizeof(keep)}),
                  "read-only Keep");
        }
        check(recv_frame(att.sock, &reply, body, &err), "read-only Keep reply");
        check(reply.type == static_cast<std::uint16_t>(FrameType::Error), "Keep denied");
        att.close_all();
        int status = 0;
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "read-only server observed denials");
        ::close(ready[0]);
        ::unlink(path.c_str());
    }

    if (g_failed == 0)
        std::printf("OK: bus transport #5124\n");
    return g_failed == 0 ? 0 : 1;
}
