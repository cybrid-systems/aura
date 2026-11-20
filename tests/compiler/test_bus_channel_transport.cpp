// Issue #5125: channel Subscribe/Reserve/Publish/CatchUp over the socket.

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

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

template <typename T> void put(std::span<std::byte> img, std::uint64_t off, const T& v) {
    std::memcpy(img.data() + off, &v, sizeof(T));
}
template <typename T> T get(std::span<const std::byte> img, std::uint64_t off) {
    T v{};
    std::memcpy(&v, img.data() + off, sizeof(T));
    return v;
}

bool build_channel_image(std::span<std::byte> img, std::string*) {
    constexpr std::uint32_t kSections = 5;
    constexpr std::uint64_t kPerChannel = 8;
    constexpr std::uint64_t kAuditSlots = 64;
    const std::uint64_t dir = 256;
    std::uint64_t off = dir + kSections * sizeof(SectionEntry);
    auto take = [&](std::uint64_t bytes) {
        const std::uint64_t at = off;
        off = (off + bytes + 7) & ~7ull;
        return at;
    };
    const std::uint64_t metrics = take(kPerChannel * sizeof(MetricRecord));
    const std::uint64_t cct = take(kPerChannel * sizeof(CctRecord));
    const std::uint64_t patches = take(kPerChannel * sizeof(PatchRecord));
    const std::uint64_t worldlines = take(kPerChannel * sizeof(WorldlineRecord));
    const std::uint64_t audit = take(kAuditSlots * sizeof(AuditRecord));
    const std::uint64_t committed = (off + 7) & ~7ull;
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
    h.committed_bytes = committed;
    h.committed_gen = 2;
    h.section_dir_off = dir;
    h.section_count = kSections;
    put(img, 0, h);

    auto entry = [&](std::uint32_t i, SectionKind kind, std::uint64_t at, std::uint32_t elem) {
        SectionEntry e{};
        e.kind = static_cast<std::uint32_t>(kind);
        e.version = 1;
        e.offset = at;
        e.element_size = elem;
        e.capacity = kPerChannel * elem;
        e.generation = 2;
        e.fingerprint = section_layout_fingerprint(kind);
        e.digest = 14695981039346656037ull; // empty range
        put(img, dir + static_cast<std::uint64_t>(i) * sizeof(SectionEntry), e);
    };
    entry(0, SectionKind::Metrics, metrics, sizeof(MetricRecord));
    entry(1, SectionKind::Cct, cct, sizeof(CctRecord));
    entry(2, SectionKind::Patches, patches, sizeof(PatchRecord));
    entry(3, SectionKind::Worldlines, worldlines, sizeof(WorldlineRecord));
    entry(4, SectionKind::Audit, audit, sizeof(AuditRecord));
    {
        SectionEntry ae = get<SectionEntry>(img, dir + 4 * sizeof(SectionEntry));
        ae.capacity = kAuditSlots * sizeof(AuditRecord);
        put(img, dir + 4 * sizeof(SectionEntry), ae);
    }
    return true;
}

std::string test_path(const char* tag) {
    return "/tmp/aura-chan-" + std::to_string(::getpid()) + "-" + tag + ".sock";
}

bool read_channel_reply(int fd, ChannelBody* out, TransportError* err) {
    FrameHeader hdr{};
    std::vector<std::byte> body(kBusMaxControlFrame);
    if (!recv_frame(fd, &hdr, body, err))
        return false;
    if (hdr.type != static_cast<std::uint16_t>(FrameType::Credit) || hdr.len < sizeof(*out))
        return false;
    std::memcpy(out, body.data(), sizeof(*out));
    return true;
}

bool read_error(int fd, int* code, TransportError* err) {
    FrameHeader hdr{};
    std::vector<std::byte> body(kBusMaxControlFrame);
    if (!recv_frame(fd, &hdr, body, err))
        return false;
    if (hdr.type != static_cast<std::uint16_t>(FrameType::Error) || hdr.len < sizeof(ErrorBody))
        return false;
    ErrorBody eb{};
    std::memcpy(&eb, body.data(), sizeof(eb));
    *code = static_cast<int>(eb.code);
    return true;
}

} // namespace

int main() {
    const std::string path = test_path("main");
    int ready[2];
    int result[2];
    check(pipe(ready) == 0 && pipe(result) == 0, "pipes");
    const pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        ::close(ready[0]);
        ::close(result[0]);
        Server server;
        TransportError err;
        if (!server.listen_at(path, 1u << 16, 77, &err))
            _exit(20);
        WorkspacePolicy policy;
        policy.allow(0, Capability{kCapAll, 0xFF, 77, 0, 0, 0});
        server.set_policy(policy);
        const char r = 'R';
        if (::write(ready[1], &r, 1) != 1)
            _exit(21);
        Attach peer;
        if (!server.accept_one(build_channel_image, &peer, &err))
            _exit(22);
        if (!server.wait_disconnect(&err))
            _exit(23);
        const std::uint64_t out = server.audit().count(AuditKind::Publish);
        if (::write(result[1], &out, sizeof(out)) != sizeof(out))
            _exit(24);
        _exit(server.audit().count(AuditKind::Publish) >= 4 ? 0 : 25);
    }
    ::close(ready[1]);
    ::close(result[1]);
    char r = 0;
    check(::read(ready[0], &r, 1) == 1, "server ready");

    Attach att;
    TransportError err;
    Client client;
    check(client.connect_to(path, 77, 0, kCapAll, &att, &err), "attach");
    auto base = std::span<std::byte>(static_cast<std::byte*>(att.base), att.size);

    auto subscribe = [&](std::uint64_t channel, ChannelBody* out) {
        LoanBody body{};
        body.channel = channel;
        check(send_frame(att.sock, FrameType::Subscribe, att.session,
                         {reinterpret_cast<const std::byte*>(&body), sizeof(body)}),
              "Subscribe");
        check(read_channel_reply(att.sock, out, &err), "Subscribe reply");
    };
    auto reserve = [&](std::uint64_t channel, std::uint64_t bytes, std::uint64_t* offset) {
        LoanBody body{};
        body.channel = channel;
        body.length = bytes;
        check(send_frame(att.sock, FrameType::Reserve, att.session,
                         {reinterpret_cast<const std::byte*>(&body), sizeof(body)}),
              "Reserve");
        FrameHeader hdr{};
        std::vector<std::byte> reply(kBusMaxControlFrame);
        check(recv_frame(att.sock, &hdr, reply, &err), "Reserve reply");
        check(hdr.type == static_cast<std::uint16_t>(FrameType::Credit), "Reserve granted");
        if (hdr.len >= sizeof(LoanBody)) {
            LoanBody loan{};
            std::memcpy(&loan, reply.data(), sizeof(loan));
            *offset = loan.offset;
        }
    };
    auto publish = [&](std::uint64_t channel, std::uint64_t offset, std::uint64_t bytes,
                       ChannelBody* out) {
        LoanBody body{};
        body.channel = channel;
        body.offset = offset;
        body.length = bytes;
        check(send_frame(att.sock, FrameType::Publish, att.session,
                         {reinterpret_cast<const std::byte*>(&body), sizeof(body)}),
              "Publish");
        check(read_channel_reply(att.sock, out, &err), "Publish reply");
    };

    // Subscribe metrics: empty snapshot.
    ChannelBody view{};
    subscribe(1, &view);
    check(view.generation == 2 && view.count == 0 && view.snapshot == 1, "metrics snapshot empty");

    // Publish one metric.
    std::uint64_t offset = 0;
    reserve(1, sizeof(MetricRecord), &offset);
    MetricRecord m{};
    m.key_sym = kImageNoIndex;
    m.kind = 1;
    m.value = 100;
    put(base, offset, m);
    publish(1, offset, sizeof(m), &view);
    // Attach's image-audit append already advanced committed_gen 2 -> 4.
    check(view.generation == 6 && view.count == 1, "metric committed at generation 6");

    // Catch up from the old cursor: one record, not a snapshot.
    CatchUpBody cu{};
    cu.channel = 1;
    cu.generation = 2;
    cu.count = 0;
    check(send_frame(att.sock, FrameType::CatchUp, att.session,
                     {reinterpret_cast<const std::byte*>(&cu), sizeof(cu)}),
          "CatchUp");
    check(read_channel_reply(att.sock, &view, &err), "CatchUp reply");
    check(view.snapshot == 0 && view.count == 1 && view.length == sizeof(MetricRecord),
          "CatchUp returns only the new record");
    check(get<MetricRecord>(base, view.offset).value == 100, "client reads the new metric");

    // Publish CCT, patch and worldline records.
    reserve(2, sizeof(CctRecord), &offset);
    CctRecord c{};
    c.parent = kImageNoParent;
    c.mid_lo = 9;
    c.node = kImageNoIndex;
    put(base, offset, c);
    publish(2, offset, sizeof(c), &view);

    reserve(3, sizeof(PatchRecord), &offset);
    PatchRecord p{};
    p.node = kImageNoIndex;
    p.field_offset = 8;
    p.new_value = 5;
    p.worldline = kImageNoIndex;
    put(base, offset, p);
    publish(3, offset, sizeof(p), &view);

    reserve(4, sizeof(WorldlineRecord), &offset);
    WorldlineRecord w{};
    w.id = 1;
    w.parent = kImageNoParent;
    w.state = static_cast<std::uint8_t>(WorldlineState::Kept);
    w.constraint_result = static_cast<std::uint8_t>(ConstraintResult::Pass);
    put(base, offset, w);
    publish(4, offset, sizeof(w), &view);

    // Capacity: metrics has 8 slots, one used. Seven more fit, the
    // ninth reservation is a hard ENOSPC.
    for (int i = 0; i < 7; ++i) {
        reserve(1, sizeof(MetricRecord), &offset);
        MetricRecord fill{};
        fill.key_sym = kImageNoIndex;
        fill.value = 200 + static_cast<std::uint64_t>(i);
        put(base, offset, fill);
        publish(1, offset, sizeof(fill), &view);
    }
    {
        LoanBody body{};
        body.channel = 1;
        body.length = sizeof(MetricRecord);
        check(send_frame(att.sock, FrameType::Reserve, att.session,
                         {reinterpret_cast<const std::byte*>(&body), sizeof(body)}),
              "Reserve over capacity");
        int code = 0;
        check(read_error(att.sock, &code, &err), "capacity error");
        check(code == ENOSPC, "capacity reports ENOSPC");
    }

    // Per-channel loan ledgers (#5125): eight outstanding loans on CCT
    // exhaust that channel only; metrics/patches still grant.
    for (int i = 0; i < 8; ++i)
        reserve(2, sizeof(CctRecord), &offset);
    {
        LoanBody body{};
        body.channel = 2;
        body.length = sizeof(CctRecord);
        check(send_frame(att.sock, FrameType::Reserve, att.session,
                         {reinterpret_cast<const std::byte*>(&body), sizeof(body)}),
              "Reserve over the CCT loan quota");
        int code = 0;
        check(read_error(att.sock, &code, &err), "loan quota error");
        check(code == EAGAIN, "CCT loan quota reports EAGAIN");
    }
    reserve(3, sizeof(PatchRecord), &offset);

    // Audit is mirrored into the image audit section (#5122/#5126): the
    // coordinator appends only in even committed generations, and the
    // section digest keeps the whole image valid.
    check(audit_count(base) >= 8, "audit mirrored into the image");
    {
        const ImageCheck vc = validate_image(base);
        check(vc.ok(), "image validates after audit appends");
    }

    att.close_all();
    std::uint64_t server_publishes = 0;
    check(::read(result[0], &server_publishes, sizeof(server_publishes)) ==
              static_cast<ssize_t>(sizeof(server_publishes)),
          "server audit count");
    check(server_publishes >= 4, "publishes audited");
    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "server clean exit");
    ::close(ready[0]);
    ::close(result[0]);
    ::unlink(path.c_str());
    if (g_failed == 0)
        std::printf("OK: bus channel transport #5125\n");
    return g_failed == 0 ? 0 : 1;
}
