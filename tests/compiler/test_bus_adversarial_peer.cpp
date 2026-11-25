// Issue #5396: the adversarial peer boundaries.
//
// A hostile peer must not take the bus down. A stalled handshake, a
// frame flood, a grant-tree abuse, a constraint-write loop, a rollback
// that would relax the gate and malformed frames are each cut off — and
// one honest session, attached before the attacks, keeps working
// throughout.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

constexpr std::uint64_t kWorkspace = 9099;

bool build_empty_image(std::span<std::byte> img, std::string*) {
    if (img.size() < kImageHeaderSize)
        return false;
    std::memset(img.data(), 0, img.size());
    ImageHeader h{};
    std::memcpy(h.magic, kImageMagic, sizeof(kImageMagic));
    h.abi_major = kImageAbiMajor;
    h.abi_minor = kBusAbiMinor;
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

// One metrics section of eight slots: enough to subscribe to and to
// publish into (the doorbell checks).
bool build_metrics_image(std::span<std::byte> img, std::string*) {
    constexpr std::uint64_t kDir = kImageHeaderSize;
    constexpr std::uint64_t kMetricsOff = kDir + sizeof(SectionEntry);
    constexpr std::uint64_t kMetricsSlots = 8;
    if (img.size() < kMetricsOff + kMetricsSlots * sizeof(MetricRecord))
        return false;
    std::memset(img.data(), 0, img.size());
    ImageHeader h{};
    std::memcpy(h.magic, kImageMagic, sizeof(h.magic));
    h.abi_major = kImageAbiMajor;
    h.abi_minor = kBusAbiMinor;
    h.endianness = static_cast<std::uint32_t>(kImageEndianMarker);
    h.header_len = kImageHeaderSize;
    h.image_size = img.size();
    const SchemaId id = header_schema_id();
    std::memcpy(h.schema_id, &id.a, 8);
    std::memcpy(h.schema_id + 8, &id.b, 8);
    h.layout_version = 1;
    h.committed_bytes = kMetricsOff + kMetricsSlots * sizeof(MetricRecord);
    h.committed_gen = 2;
    h.section_dir_off = kDir;
    h.section_count = 1;
    std::memcpy(img.data(), &h, sizeof(h));
    SectionEntry e{};
    e.kind = static_cast<std::uint32_t>(SectionKind::Metrics);
    e.version = 1;
    e.offset = kMetricsOff;
    e.element_size = sizeof(MetricRecord);
    e.capacity = kMetricsSlots * sizeof(MetricRecord);
    e.generation = 2;
    e.fingerprint = section_layout_fingerprint(SectionKind::Metrics);
    e.digest = 14695981039346656037ull;
    std::memcpy(img.data() + kDir, &e, sizeof(e));
    return true;
}

// A raw connection: the attacker speaks the wire protocol by hand.
int raw_connect(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Read one frame; false on EOF (the coordinator dropped the session).
bool read_frame(int fd, FrameHeader* hdr, std::vector<std::byte>* body, TransportError* err) {
    body->assign(kBusMaxControlFrame, std::byte{0});
    return recv_frame(fd, hdr, *body, err);
}

std::string error_message(const std::vector<std::byte>& body) {
    if (body.size() < sizeof(ErrorBody))
        return std::string();
    ErrorBody eb{};
    std::memcpy(&eb, body.data(), sizeof(eb));
    return std::string(eb.message, strnlen(eb.message, sizeof(eb.message)));
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// The honest peer: one session that must survive every attack. The
// round query is a read that always answers.
struct Honest {
    Client client;
    Attach att{};

    bool attach(const std::string& path, TransportError* err) {
        return client.connect_to(path, kWorkspace, 0, kCapRead | kCapPublish, &att, err);
    }

    bool alive(TransportError* err) {
        RoundRequestBody request{};
        if (!send_frame(att.sock, FrameType::RoundQuery, att.session,
                        {reinterpret_cast<const std::byte*>(&request), sizeof(request)}))
            return false;
        FrameHeader hdr{};
        std::vector<std::byte> reply;
        if (!read_frame(att.sock, &hdr, &reply, err))
            return false;
        return hdr.type == static_cast<std::uint16_t>(FrameType::Credit);
    }
};

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-bus-adv-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    const std::string path = std::string(dir) + "/bus.sock";

    Server server;
    TransportError err;
    check(server.listen_at(path, 1u << 16, kWorkspace, &err), "listen");
    WorkspacePolicy policy;
    policy.allow(0, Capability{kCapAll, 0xFF, kWorkspace, 0, 0, 0});
    policy.allow(1, Capability{kCapRead | kCapPublish | kCapTrial | kCapGrant | kCapConstraintWrite,
                               0xFF, kWorkspace, 0, 0, 0});
    server.set_policy(policy);
    server.register_constraint_predicate(
        "adv-rule", [](const ConstraintContext&, std::string*) { return true; });
    // The quotas are tight on purpose: the test must trip them fast.
    PeerQuota quota;
    quota.frame_rate = 100;
    quota.frame_burst = 64;
    quota.frame_burst_explicit = true;
    quota.hello_timeout_ms = 400;
    quota.constraint_changes_per_window = 3;
    server.set_peer_quota(quota);

    std::thread coordinator([&] { server.serve(build_empty_image); });
    sleep_ms(200);

    // The honest peer attaches first and stays for the whole run.
    Honest honest;
    check(honest.attach(path, &err), "the honest peer attaches");
    {
        TransportError e{};
        check(honest.alive(&e), "the honest peer gets an answer");
    }

    // ── 1. A peer that connects and never says hello ────────────
    {
        const int stalled = raw_connect(path);
        check(stalled >= 0, "the stalled peer connects");
        sleep_ms(900); // the handshake deadline is 400ms
        struct pollfd pfd{stalled, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 1000);
        check(pr > 0 && (pfd.revents & (POLLIN | POLLHUP)) != 0,
              "the stalled handshake is dropped");
        ::close(stalled);
        TransportError e{};
        check(honest.alive(&e), "the honest peer survives the stalled handshake");
    }

    // ── 2. A peer that floods the control plane ─────────────────
    {
        Client flooder;
        Attach att{};
        check(flooder.connect_to(path, kWorkspace, 0, kCapRead, &att, &err),
              "the flooder attaches");
        LoanBody body{};
        body.channel = 1;
        for (int i = 0; i < 96; ++i) {
            send_frame(att.sock, FrameType::Subscribe, att.session,
                       {reinterpret_cast<const std::byte*>(&body), sizeof(body)});
        }
        // The bucket holds 64 frames: the session is cut off.
        bool dropped = false;
        std::string cut_reason;
        for (int i = 0; i < 96 && !dropped; ++i) {
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            if (!read_frame(att.sock, &hdr, &reply, &err))
                dropped = true;
            else if (hdr.type == static_cast<std::uint16_t>(FrameType::Error)) {
                // #5426: the cut is announced with a rate-limit error
                // before the session is closed. Other dispatch errors
                // (the empty test image has no channel section) do not
                // end the session.
                const std::string message = error_message(reply);
                if (message.find("rate-limited") != std::string::npos) {
                    dropped = true;
                    cut_reason = message;
                }
            }
        }
        check(dropped, "the flood is cut off");
        check(cut_reason.find("rate-limited") != std::string::npos, "the cut names the rate limit");
        ::close(att.sock);
        TransportError e{};
        check(honest.alive(&e), "the honest peer survives the flood");
    }

    // ── 3. The grant tree's limits ──────────────────────────────
    {
        Client granter;
        Attach att{};
        check(granter.connect_to(path, kWorkspace, 1, kCapGrant | kCapRead, &att, &err),
              "the granter attaches");
        std::uint64_t parent = 0;
        bool refused = false;
        std::string why;
        for (int depth = 0; depth < 8; ++depth) {
            GrantBody request{};
            request.parent = parent;
            request.caps = Capability{kCapRead | kCapGrant, 0xFF, kWorkspace, 0, 0, 0};
            if (!send_frame(att.sock, FrameType::Grant, att.session,
                            {reinterpret_cast<const std::byte*>(&request), sizeof(request)})) {
                refused = true;
                break;
            }
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            if (!read_frame(att.sock, &hdr, &reply, &err)) {
                refused = true;
                break;
            }
            if (hdr.type == static_cast<std::uint16_t>(FrameType::Error)) {
                refused = true;
                why = error_message(reply);
                break;
            }
            GrantReply granted{};
            std::memcpy(&granted, reply.data(), sizeof(granted));
            parent = granted.token;
        }
        check(refused, "the grant tree refuses an over-deep chain");
        check(why.find("grant depth limit") != std::string::npos,
              "the refusal names the depth limit");
        ::close(att.sock);
        TransportError e{};
        check(honest.alive(&e), "the honest peer survives the grant abuse");
    }

    // ── 3b. The grant fanout limit ─────────────────────────────
    {
        Client granter;
        Attach att{};
        check(granter.connect_to(path, kWorkspace, 1, kCapGrant | kCapRead, &att, &err),
              "the granter attaches again");
        bool refused = false;
        std::string why;
        for (int i = 0; i < 32 && !refused; ++i) {
            GrantBody request{};
            request.parent = 0; // the session's own root
            request.caps = Capability{kCapRead | kCapGrant, 0xFF, kWorkspace, 0, 0, 0};
            if (!send_frame(att.sock, FrameType::Grant, att.session,
                            {reinterpret_cast<const std::byte*>(&request), sizeof(request)}))
                break;
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            if (!read_frame(att.sock, &hdr, &reply, &err))
                break;
            if (hdr.type == static_cast<std::uint16_t>(FrameType::Error)) {
                refused = true;
                why = error_message(reply);
            }
        }
        check(refused, "the grant tree refuses an over-wide fanout");
        check(why.find("grant fanout limit") != std::string::npos,
              "the refusal names the fanout limit");
        ::close(att.sock);
        TransportError e{};
        check(honest.alive(&e), "the honest peer survives the grant fanout");
    }

    // ── 4. A rollback may not relax the gate ────────────────────
    {
        const std::string name = "adv-rule";
        const std::string source = "(lambda (ctx) #t)";
        ConstraintWire wire{static_cast<std::uint32_t>(name.size()),
                            static_cast<std::uint32_t>(source.size())};
        std::vector<std::byte> payload(sizeof(wire) + name.size() + source.size());
        std::memcpy(payload.data(), &wire, sizeof(wire));
        std::memcpy(payload.data() + sizeof(wire), name.data(), name.size());
        std::memcpy(payload.data() + sizeof(wire) + name.size(), source.data(), source.size());

        Client owner;
        Attach owner_att{};
        check(owner.connect_to(path, kWorkspace, 1, kCapConstraintWrite, &owner_att, &err),
              "the rule owner attaches");
        check(send_frame(owner_att.sock, FrameType::ConstraintInstall, owner_att.session, payload),
              "the owner installs the rule");
        {
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            check(read_frame(owner_att.sock, &hdr, &reply, &err), "the install reply");
            check(hdr.type != static_cast<std::uint16_t>(FrameType::Error),
                  "the owner may install the rule");
        }

        // Another session with the same authority may not undo it.
        Client other;
        Attach other_att{};
        check(other.connect_to(path, kWorkspace, 1, kCapConstraintWrite, &other_att, &err),
              "the second writer attaches");
        check(send_frame(other_att.sock, FrameType::ConstraintRollback, other_att.session, {}),
              "the second writer asks for a rollback");
        {
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            check(read_frame(other_att.sock, &hdr, &reply, &err), "the rollback reply");
            check(hdr.type == static_cast<std::uint16_t>(FrameType::Error),
                  "the rollback is refused");
            check(error_message(reply).find("relax the protection set") != std::string::npos,
                  "the refusal names the protection set");
        }
        ::close(owner_att.sock);
        ::close(other_att.sock);
        TransportError e{};
        check(honest.alive(&e), "the honest peer survives the rollback attempt");
    }

    // ── 5. The constraint-change quota ──────────────────────────
    {
        const std::string name = "adv-rule";
        const std::string source = "(lambda (ctx) #t)";
        ConstraintWire wire{static_cast<std::uint32_t>(name.size()),
                            static_cast<std::uint32_t>(source.size())};
        std::vector<std::byte> payload(sizeof(wire) + name.size() + source.size());
        std::memcpy(payload.data(), &wire, sizeof(wire));
        std::memcpy(payload.data() + sizeof(wire), name.data(), name.size());
        std::memcpy(payload.data() + sizeof(wire) + name.size(), source.data(), source.size());
        Client writer;
        Attach att{};
        check(writer.connect_to(path, kWorkspace, 1, kCapConstraintWrite, &att, &err),
              "the writer attaches");
        bool throttled = false;
        std::string why;
        for (int i = 0; i < 8 && !throttled; ++i) {
            const bool install = (i % 2) == 0;
            if (!send_frame(att.sock,
                            install ? FrameType::ConstraintInstall : FrameType::ConstraintRollback,
                            att.session, install ? payload : std::vector<std::byte>{}))
                break;
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            if (!read_frame(att.sock, &hdr, &reply, &err))
                break;
            if (hdr.type == static_cast<std::uint16_t>(FrameType::Error)) {
                why = error_message(reply);
                // The protection refusal is not the quota — keep going
                // until the quota speaks.
                if (why.find("rate-limited") != std::string::npos)
                    throttled = true;
            }
        }
        check(throttled, "the constraint-write loop is throttled");
        check(why.find("rate-limited") != std::string::npos, "the refusal names the quota");
        ::close(att.sock);
        TransportError e{};
        check(honest.alive(&e), "the honest peer survives the constraint-write loop");
    }

    // ── 5b. A peer that hogs its staging window ─────────────────
    {
        Client hog;
        Attach att{};
        check(hog.connect_to(path, kWorkspace, 0, kCapPublish, &att, &err), "the hog attaches");
        LoanBody body{};
        body.channel = 0; // not a channel section: the staging window
        body.length = 32 * 1024;
        check(send_frame(att.sock, FrameType::Reserve, att.session,
                         {reinterpret_cast<const std::byte*>(&body), sizeof(body)}),
              "the hog reserves");
        {
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            check(read_frame(att.sock, &hdr, &reply, &err), "the reserve reply");
            check(hdr.type == static_cast<std::uint16_t>(FrameType::Credit),
                  "the hog gets its window");
        }
        check(send_frame(att.sock, FrameType::Reserve, att.session,
                         {reinterpret_cast<const std::byte*>(&body), sizeof(body)}),
              "the hog reserves again");
        {
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            check(read_frame(att.sock, &hdr, &reply, &err), "the second reserve reply");
            check(hdr.type == static_cast<std::uint16_t>(FrameType::Error),
                  "the staging window is exhausted for the hog");
        }
        // The honest peer has its own window: it is not starved.
        Client other;
        Attach other_att{};
        check(other.connect_to(path, kWorkspace, 0, kCapPublish, &other_att, &err),
              "the second publisher attaches");
        LoanBody small{};
        small.channel = 0;
        small.length = 1024;
        check(send_frame(other_att.sock, FrameType::Reserve, other_att.session,
                         {reinterpret_cast<const std::byte*>(&small), sizeof(small)}),
              "the honest publisher reserves");
        {
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            check(read_frame(other_att.sock, &hdr, &reply, &err), "the honest reserve reply");
            check(hdr.type == static_cast<std::uint16_t>(FrameType::Credit),
                  "the honest publisher is not starved by the hog");
        }
        ::close(other_att.sock);
        ::close(att.sock);
        TransportError e{};
        check(honest.alive(&e), "the honest peer survives the staging hog");
    }

    // ── 6. Malformed frames ─────────────────────────────────────
    {
        // A frame with an absurd length.
        {
            Client mal;
            Attach att{};
            check(mal.connect_to(path, kWorkspace, 0, kCapRead, &att, &err),
                  "the malformed peer attaches");
            FrameHeader hdr{};
            hdr.len = 0xFFFFFFFFu;
            hdr.type = static_cast<std::uint16_t>(FrameType::Subscribe);
            hdr.session = att.session;
            check(::write(att.sock, &hdr, sizeof(hdr)) == static_cast<ssize_t>(sizeof(hdr)),
                  "write the oversized header");
            struct pollfd pfd{att.sock, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 2000);
            check(pr > 0, "the coordinator reacts to the oversized frame");
            ::close(att.sock);
        }
        // An unknown frame type and a truncated body.
        {
            Client mal;
            Attach att{};
            check(mal.connect_to(path, kWorkspace, 0, kCapRead, &att, &err),
                  "the malformed peer attaches again");
            FrameHeader hdr{};
            hdr.len = 2;
            hdr.type = 0xBEEF;
            hdr.session = att.session;
            const std::uint8_t junk[2] = {0xAA, 0xBB};
            check(::write(att.sock, &hdr, sizeof(hdr)) == static_cast<ssize_t>(sizeof(hdr)),
                  "write the bogus header");
            check(::write(att.sock, junk, sizeof(junk)) == static_cast<ssize_t>(sizeof(junk)),
                  "write the junk body");
            // The session survives: the next well-formed frame is answered.
            RoundRequestBody request{};
            check(send_frame(att.sock, FrameType::RoundQuery, att.session,
                             {reinterpret_cast<const std::byte*>(&request), sizeof(request)}),
                  "the malformed peer asks again");
            FrameHeader reply_hdr{};
            std::vector<std::byte> reply;
            check(read_frame(att.sock, &reply_hdr, &reply, &err), "the coordinator still answers");
            check(reply_hdr.type == static_cast<std::uint16_t>(FrameType::Credit),
                  "the unknown frame type did not break the session");
            ::close(att.sock);
        }
        // An illegal channel and a truncated publish.
        {
            Client mal;
            Attach att{};
            check(mal.connect_to(path, kWorkspace, 0, kCapRead | kCapPublish, &att, &err),
                  "the malformed peer attaches a third time");
            LoanBody body{};
            body.channel = 99; // no such channel
            check(send_frame(att.sock, FrameType::Publish, att.session,
                             {reinterpret_cast<const std::byte*>(&body), sizeof(body)}),
                  "the malformed peer publishes to nowhere");
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            check(read_frame(att.sock, &hdr, &reply, &err), "the illegal channel is refused");
            check(hdr.type == static_cast<std::uint16_t>(FrameType::Error),
                  "the illegal channel is an error");
            ::close(att.sock);
        }
        TransportError e{};
        check(honest.alive(&e), "the honest peer survives the malformed frames");
    }

    // ── The honest peer still writes ────────────────────────────
    {
        TransportError e{};
        LoanBody body{};
        body.channel = 0; // the staging window, not a channel section
        body.length = 64;
        check(send_frame(honest.att.sock, FrameType::Reserve, honest.att.session,
                         {reinterpret_cast<const std::byte*>(&body), sizeof(body)}),
              "the honest peer reserves");
        FrameHeader hdr{};
        std::vector<std::byte> reply;
        check(read_frame(honest.att.sock, &hdr, &reply, &e), "the honest reserve reply");
        check(hdr.type == static_cast<std::uint16_t>(FrameType::Credit),
              "the honest peer gets its window");
        LoanBody granted{};
        if (reply.size() >= sizeof(granted))
            std::memcpy(&granted, reply.data(), sizeof(granted));
        check(send_frame(honest.att.sock, FrameType::Publish, honest.att.session,
                         {reinterpret_cast<const std::byte*>(&granted), sizeof(granted)}),
              "the honest peer publishes");
        check(read_frame(honest.att.sock, &hdr, &reply, &e), "the honest publish reply");
        check(hdr.type == static_cast<std::uint16_t>(FrameType::Credit),
              "the honest publish is accepted");
        // The credit returns the loan, so the next round starts clean.
        // The coordinator does not answer a credit.
        check(send_frame(honest.att.sock, FrameType::Credit, honest.att.session,
                         {reinterpret_cast<const std::byte*>(&granted), sizeof(granted)}),
              "the honest peer returns the loan");
    }

    // ── 8. Frames in pieces, and frames together ───────────────
    // poll reports readability, never a whole frame. The coordinator
    // serves a Hello and a frame that each arrive in two writes, answers
    // two frames written back to back in order, and serves a frame that
    // trails the Hello in the same write.
    {
        const auto put_frame = [](std::vector<std::byte>& out, FrameType type,
                                  std::span<const std::byte> body) {
            FrameHeader header{};
            header.len = static_cast<std::uint32_t>(body.size());
            header.type = static_cast<std::uint16_t>(type);
            const std::size_t at = out.size();
            out.resize(at + sizeof(header) + body.size());
            std::memcpy(out.data() + at, &header, sizeof(header));
            std::memcpy(out.data() + at + sizeof(header), body.data(), body.size());
        };
        const auto write_bytes = [](int fd, const std::byte* data, std::size_t bytes) {
            while (bytes > 0) {
                const ssize_t n = ::send(fd, data, bytes, MSG_NOSIGNAL);
                if (n <= 0)
                    return false;
                data += n;
                bytes -= static_cast<std::size_t>(n);
            }
            return true;
        };
        // True while the coordinator stays silent.
        const auto silent = [](int fd) {
            struct pollfd pfd{fd, POLLIN, 0};
            return ::poll(&pfd, 1, 150) == 0;
        };
        HelloBody hello{};
        hello.abi_major = kBusAbiMajor;
        hello.abi_minor = kBusAbiMinor;
        const SchemaId id = header_schema_id();
        std::memcpy(hello.schema_id, &id.a, 8);
        std::memcpy(hello.schema_id + 8, &id.b, 8);
        hello.layout_version = 1;
        hello.role = 0;
        hello.caps = kCapRead;
        hello.channels = ~0ull;
        hello.workspace_id = kWorkspace;
        std::vector<std::byte> hello_frame;
        put_frame(hello_frame, FrameType::Hello,
                  {reinterpret_cast<const std::byte*>(&hello), sizeof(hello)});
        RoundRequestBody round{};
        std::vector<std::byte> round_query;
        put_frame(round_query, FrameType::RoundQuery,
                  {reinterpret_cast<const std::byte*>(&round), sizeof(round)});
        DegradeQueryBody degrade{};
        std::vector<std::byte> degrade_query;
        put_frame(degrade_query, FrameType::DegradeQuery,
                  {reinterpret_cast<const std::byte*>(&degrade), sizeof(degrade)});

        // A Hello in two writes: nothing answers the first half.
        const int split = raw_connect(path);
        check(split >= 0, "the piecemeal peer connects");
        check(write_bytes(split, hello_frame.data(), hello_frame.size() / 2),
              "half the Hello is sent");
        check(silent(split), "half a Hello is not answered");
        check(write_bytes(split, hello_frame.data() + hello_frame.size() / 2,
                          hello_frame.size() - hello_frame.size() / 2),
              "the rest of the Hello is sent");
        {
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            check(read_frame(split, &hdr, &reply, &err), "the piecemeal Hello is answered");
            check(hdr.type == static_cast<std::uint16_t>(FrameType::HelloOk),
                  "a Hello in two writes attaches");
        }
        // A frame in two writes: nothing answers the first piece.
        check(write_bytes(split, round_query.data(), 20), "the first piece of a frame is sent");
        check(silent(split), "a partial frame is not answered");
        check(write_bytes(split, round_query.data() + 20, round_query.size() - 20),
              "the rest of the frame is sent");
        {
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            check(read_frame(split, &hdr, &reply, &err), "the frame in two writes is answered");
            check(hdr.type == static_cast<std::uint16_t>(FrameType::Credit) &&
                      hdr.len == sizeof(RoundReplyBody),
                  "a frame in two writes is served");
        }
        // Two frames in one write: two answers, in order (the round reply
        // is 120 bytes, the degrade reply 112).
        std::vector<std::byte> pair = round_query;
        pair.insert(pair.end(), degrade_query.begin(), degrade_query.end());
        check(write_bytes(split, pair.data(), pair.size()), "two frames are sent together");
        {
            FrameHeader first{};
            FrameHeader second{};
            std::vector<std::byte> reply;
            check(read_frame(split, &first, &reply, &err), "the first of the pair is answered");
            check(read_frame(split, &second, &reply, &err), "the second of the pair is answered");
            check(first.len == sizeof(RoundReplyBody) && second.len == sizeof(DegradeReplyBody),
                  "frames sent together are answered in order");
        }
        ::close(split);

        // A frame that trails the Hello in the same write, then a frame
        // that arrives in pieces after it.
        const int trailing = raw_connect(path);
        check(trailing >= 0, "the trailing-frame peer connects");
        std::vector<std::byte> burst = hello_frame;
        burst.insert(burst.end(), round_query.begin(), round_query.end());
        burst.insert(burst.end(), degrade_query.begin(), degrade_query.begin() + 10);
        check(write_bytes(trailing, burst.data(), burst.size()),
              "the Hello, a frame and a fragment are sent together");
        {
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            check(read_frame(trailing, &hdr, &reply, &err), "the trailing Hello is answered");
            check(hdr.type == static_cast<std::uint16_t>(FrameType::HelloOk),
                  "a Hello followed by frames attaches");
            check(read_frame(trailing, &hdr, &reply, &err),
                  "the frame after the Hello is answered");
            check(hdr.len == sizeof(RoundReplyBody), "the frame after the Hello is served");
            check(silent(trailing), "the fragment after the Hello is not answered");
            check(write_bytes(trailing, degrade_query.data() + 10, degrade_query.size() - 10),
                  "the rest of the fragment is sent");
            check(read_frame(trailing, &hdr, &reply, &err),
                  "the fragmented frame after the Hello is answered");
            check(hdr.len == sizeof(DegradeReplyBody),
                  "the fragmented frame after the Hello is served");
        }
        ::close(trailing);
        TransportError e{};
        check(honest.alive(&e), "the honest peer survives the piecemeal frames");
    }

    // ── The audit records every cut-off ─────────────────────────
    server.stop();
    coordinator.join();
    {
        std::size_t rate_limited = 0;
        std::size_t grant_limited = 0;
        bool hello_deadline = false;
        bool frame_quota = false;
        for (const auto& event : server.audit().events()) {
            if (event.kind == AuditKind::RateLimited) {
                ++rate_limited;
                if (event.reason.find("handshake deadline") != std::string::npos)
                    hello_deadline = true;
                if (event.reason.find("frame quota") != std::string::npos)
                    frame_quota = true;
            }
            if (event.kind == AuditKind::GrantLimited)
                ++grant_limited;
        }
        check(hello_deadline, "the stalled handshake is audited");
        check(frame_quota, "the frame flood is audited");
        check(rate_limited >= 3, "every quota cut-off is audited");
        check(grant_limited >= 1, "the grant limit is audited");
    }

    // ── 9. Doorbells reach the subscribers, once per turn ───────
    // A commit rings the eventfd of the sessions that subscribed and of
    // nobody else; the commits of one poll turn are one ring.
    {
        const std::string bell_path = std::string(dir) + "/bell.sock";
        Server bells;
        check(bells.listen_at(bell_path, 1u << 16, kWorkspace, &err), "the doorbell bus listens");
        WorkspacePolicy policy;
        policy.allow(0, Capability{kCapAll, 0xFF, kWorkspace, 0, 0, 0});
        bells.set_policy(policy);
        std::thread bell_thread([&] { bells.serve(build_metrics_image); });
        sleep_ms(200);

        // The doorbell's counter, or -1 while it stays silent.
        const auto bell_count = [](int fd, int timeout_ms) -> long {
            struct pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, timeout_ms) <= 0)
                return -1;
            std::uint64_t count = 0;
            if (::read(fd, &count, sizeof(count)) != static_cast<ssize_t>(sizeof(count)))
                return -1;
            return static_cast<long>(count);
        };
        const auto send_loan_body = [&](Attach& peer, FrameType type, const LoanBody& loan) {
            check(send_frame(peer.sock, type, peer.session,
                             {reinterpret_cast<const std::byte*>(&loan), sizeof(loan)}),
                  "the doorbell frame is sent");
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            check(read_frame(peer.sock, &hdr, &reply, &err) &&
                      hdr.type == static_cast<std::uint16_t>(FrameType::Credit),
                  "the doorbell frame is answered");
        };
        const auto publish = [&](Attach& peer) {
            LoanBody reserve{};
            reserve.channel = 1;
            reserve.length = sizeof(MetricRecord);
            check(send_frame(peer.sock, FrameType::Reserve, peer.session,
                             {reinterpret_cast<const std::byte*>(&reserve), sizeof(reserve)}),
                  "Reserve");
            FrameHeader hdr{};
            std::vector<std::byte> reply;
            check(read_frame(peer.sock, &hdr, &reply, &err) &&
                      hdr.type == static_cast<std::uint16_t>(FrameType::Credit),
                  "Reserve reply");
            LoanBody granted{};
            std::memcpy(&granted, reply.data(), sizeof(granted));
            MetricRecord m{};
            m.key_sym = kImageNoIndex;
            m.value = 1;
            std::memcpy(static_cast<std::byte*>(peer.base) + granted.offset, &m, sizeof(m));
            LoanBody pub = granted;
            pub.channel = 1;
            pub.length = sizeof(m);
            send_loan_body(peer, FrameType::Publish, pub);
        };

        Client watcher_client, plain_client, actor_client;
        Attach watcher{}, plain{}, actor{};
        check(watcher_client.connect_to(bell_path, kWorkspace, 0, kCapRead, &watcher, &err),
              "the subscriber attaches");
        check(plain_client.connect_to(bell_path, kWorkspace, 0, kCapRead, &plain, &err),
              "the reader that never subscribes attaches");
        check(actor_client.connect_to(bell_path, kWorkspace, 0, kCapPublish | kCapTrial, &actor,
                                      &err),
              "the publisher attaches");
        LoanBody subscription{};
        subscription.channel = 1;
        send_loan_body(watcher, FrameType::Subscribe, subscription);

        publish(actor);
        check(bell_count(watcher.event, 2000) == 1, "the subscriber's doorbell rings once");
        check(bell_count(plain.event, 150) < 0, "a session that never subscribed is not rung");
        check(bell_count(actor.event, 150) < 0, "the publisher is not rung for its own commit");

        // Five trials written back to back are one poll turn: one ring.
        {
            std::vector<std::byte> burst;
            for (int i = 0; i < 5; ++i) {
                FrameHeader hdr{};
                hdr.len = sizeof(LoanBody);
                hdr.type = static_cast<std::uint16_t>(FrameType::Trial);
                hdr.session = actor.session;
                const std::size_t at = burst.size();
                burst.resize(at + sizeof(hdr) + sizeof(LoanBody));
                std::memcpy(burst.data() + at, &hdr, sizeof(hdr));
                LoanBody trial{};
                trial.channel = 1;
                std::memcpy(burst.data() + at + sizeof(hdr), &trial, sizeof(trial));
            }
            check(::send(actor.sock, burst.data(), burst.size(), MSG_NOSIGNAL) ==
                      static_cast<ssize_t>(burst.size()),
                  "five trials in one write");
            for (int i = 0; i < 5; ++i) {
                FrameHeader hdr{};
                std::vector<std::byte> reply;
                check(read_frame(actor.sock, &hdr, &reply, &err) &&
                          hdr.type == static_cast<std::uint16_t>(FrameType::Credit),
                      "each trial of the burst is answered");
            }
        }
        check(bell_count(watcher.event, 2000) == 1, "a turn of commits is one ring");

        // The doorbell stops with the subscription.
        LoanBody unsubscribe{};
        unsubscribe.channel = 1;
        send_loan_body(watcher, FrameType::Unsubscribe, unsubscribe);
        publish(actor);
        check(bell_count(watcher.event, 150) < 0, "the doorbell stops with the subscription");

        watcher.close_all();
        plain.close_all();
        actor.close_all();
        bells.stop();
        bell_thread.join();
        ::unlink(bell_path.c_str());
    }

    ::unlink(path.c_str());
    if (g_failed == 0)
        std::printf("test_bus_adversarial_peer: OK\n");
    return g_failed == 0 ? 0 : 1;
}
