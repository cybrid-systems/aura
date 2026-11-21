// Issue #5125: incremental channels over a committed image — two
// processes, four channels, cursor diffs, capacity exhaustion.

#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus/channel.hh"
#include "bus/image_schema.hh"

namespace {

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

using namespace aura::bus;

std::uint64_t align8(std::uint64_t v) {
    return (v + 7) & ~7ull;
}

template <typename T> void put(std::span<std::byte> img, std::uint64_t off, const T& v) {
    std::memcpy(img.data() + off, &v, sizeof(T));
}
template <typename T> T get(std::span<const std::byte> img, std::uint64_t off) {
    T v{};
    std::memcpy(&v, img.data() + off, sizeof(T));
    return v;
}

std::uint64_t fnv(std::span<const std::byte> img, std::uint64_t off, std::uint64_t len) {
    std::uint64_t h = 14695981039346656037ull;
    for (std::uint64_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(img[off + i]));
        h *= 1099511628211ull;
    }
    return h;
}

struct Layout {
    std::uint64_t strings = 0, symbols = 0, nodes = 0;
    std::uint64_t metrics = 0, cct = 0, patches = 0, worldlines = 0;
    std::uint64_t committed = 0;
};

Layout build(std::span<std::byte> img) {
    Layout L{};
    constexpr std::uint32_t kSections = 7;
    const std::uint64_t dir = 256;
    std::uint64_t off = dir + kSections * sizeof(SectionEntry);
    auto take = [&](std::uint64_t bytes) {
        const std::uint64_t at = off;
        off = align8(off + bytes);
        return at;
    };
    L.strings = take(24);
    L.symbols = take(sizeof(SymbolEntry));
    L.nodes = take(4 * sizeof(NodeRecord));
    L.metrics = take(8 * sizeof(MetricRecord));
    L.cct = take(8 * sizeof(CctRecord));
    L.patches = take(8 * sizeof(PatchRecord));
    L.worldlines = take(8 * sizeof(WorldlineRecord));
    L.committed = align8(off);
    std::memset(img.data(), 0, L.committed);

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
    h.committed_bytes = L.committed;
    h.committed_gen = 2;
    h.section_dir_off = dir;
    h.section_count = kSections;
    put(img, 0, h);

    auto entry = [&](std::uint32_t i, SectionKind kind, std::uint64_t at, std::uint32_t elem,
                     std::uint32_t count, std::uint64_t capacity) {
        SectionEntry e{};
        e.kind = static_cast<std::uint32_t>(kind);
        e.version = 1;
        e.offset = at;
        e.length = static_cast<std::uint64_t>(count) * elem;
        e.element_size = elem;
        e.count = count;
        e.capacity = capacity;
        e.generation = 2;
        e.fingerprint = section_layout_fingerprint(kind);
        put(img, dir + static_cast<std::uint64_t>(i) * sizeof(SectionEntry), e);
    };
    entry(0, SectionKind::StringTable, L.strings, sizeof(StringTableEntry), 1, 24);
    entry(1, SectionKind::SymbolTable, L.symbols, sizeof(SymbolEntry), 1, sizeof(SymbolEntry));
    entry(2, SectionKind::Nodes, L.nodes, sizeof(NodeRecord), 1, 4 * sizeof(NodeRecord));
    entry(3, SectionKind::Metrics, L.metrics, sizeof(MetricRecord), 0, 8 * sizeof(MetricRecord));
    entry(4, SectionKind::Cct, L.cct, sizeof(CctRecord), 0, 8 * sizeof(CctRecord));
    entry(5, SectionKind::Patches, L.patches, sizeof(PatchRecord), 0, 8 * sizeof(PatchRecord));
    entry(6, SectionKind::Worldlines, L.worldlines, sizeof(WorldlineRecord), 0,
          8 * sizeof(WorldlineRecord));

    put(img, L.strings, StringTableEntry{16, 3, 0});
    std::memcpy(img.data() + L.strings + 16, "hot", 3);
    put(img, L.symbols, SymbolEntry{16, 3, 0});

    NodeRecord n{};
    n.kind = 1;
    n.slot = 0;
    n.sym = 0;
    n.parent = kImageNoIndex;
    n.first_child = kImageNoIndex;
    n.next_sibling = kImageNoIndex;
    put(img, L.nodes, n);
    // The string section carries entries plus inline bytes.
    {
        SectionEntry se = get<SectionEntry>(img, dir);
        se.length = 24;
        put(img, dir, se);
    }

    // Commit digests after the section bytes exist.
    for (std::uint32_t i = 0; i < kSections; ++i) {
        const std::uint64_t at = dir + static_cast<std::uint64_t>(i) * sizeof(SectionEntry);
        SectionEntry e = get<SectionEntry>(img, at);
        e.digest = fnv(img, e.offset, e.length);
        put(img, at, e);
    }
    return L;
}

std::uint64_t write_metric(std::span<std::byte> img, std::uint64_t value) {
    std::uint64_t offset = 0;
    check(channel_reserve(img, Channel::Metrics, sizeof(MetricRecord), &offset, nullptr),
          "reserve metric");
    MetricRecord m{};
    m.key_sym = 0;
    m.kind = 1;
    m.value = value;
    put(img, offset, m);
    std::uint64_t gen = 0;
    check(channel_commit(img, Channel::Metrics, offset, sizeof(m), &gen, nullptr), "commit metric");
    return gen;
}

std::uint64_t digest_channels(std::span<const std::byte> img) {
    std::uint64_t h = 0;
    for (Channel ch : {Channel::Metrics, Channel::Cct, Channel::Patches, Channel::Worldlines}) {
        ChannelCursor cursor;
        std::span<const std::byte> recs;
        bool snapshot = false;
        if (!channel_read(img, ch, &cursor, &recs, &snapshot, nullptr))
            return 0;
        for (std::size_t i = 0; i < recs.size(); ++i)
            h = (h * 131) ^ static_cast<unsigned char>(recs[i]);
        h = (h * 131) ^ cursor.count;
    }
    return h;
}

} // namespace

int main() {
    constexpr std::uint64_t kSize = 1u << 16;
    const int fd = static_cast<int>(syscall(SYS_memfd_create, "channels", 0));
    check(fd >= 0, "memfd");
    check(ftruncate(fd, kSize) == 0, "ftruncate");
    void* map = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(map != MAP_FAILED, "mmap");
    auto img = std::span<std::byte>(static_cast<std::byte*>(map), kSize);
    std::memset(map, 0, kSize);
    (void)build(img);
    {
        const auto vc = validate_image(img);
        if (!vc.ok())
            std::printf("channel image: %s (%s)\n",
                        std::string(image_status_name(vc.status)).c_str(), vc.detail);
        check(vc.ok(), "channel image validates");
    }

    int up[2];
    int down[2];
    check(pipe(up) == 0 && pipe(down) == 0, "pipes");
    const pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        ::close(up[1]);
        ::close(down[0]);
        void* ro = mmap(nullptr, kSize, PROT_READ, MAP_SHARED, fd, 0);
        if (ro == MAP_FAILED)
            _exit(10);
        auto view = std::span<const std::byte>(static_cast<const std::byte*>(ro), kSize);
        for (int phase = 0; phase < 2; ++phase) {
            char cmd = 0;
            if (::read(up[0], &cmd, 1) != 1)
                _exit(11);
            if (phase == 0) {
                ChannelCursor mc, cc, pc, wc;
                std::span<const std::byte> mrecs, crecs, precs, wrecs;
                bool snap = false;
                if (!channel_read(view, Channel::Metrics, &mc, &mrecs, &snap, nullptr) ||
                    !channel_read(view, Channel::Cct, &cc, &crecs, &snap, nullptr) ||
                    !channel_read(view, Channel::Patches, &pc, &precs, &snap, nullptr) ||
                    !channel_read(view, Channel::Worldlines, &wc, &wrecs, &snap, nullptr))
                    _exit(12);
                MetricRecord m = get<MetricRecord>(mrecs, 0);
                CctRecord c = get<CctRecord>(crecs, 0);
                PatchRecord p = get<PatchRecord>(precs, 0);
                WorldlineRecord w = get<WorldlineRecord>(wrecs, 0);
                std::uint64_t h = 14695981039346656037ull;
                h = h * 131 + m.value;
                h = h * 131 + c.node;
                h = h * 131 + p.node;
                h = h * 131 + w.id;
                if (m.key_sym != 0 || c.node != 0 || p.node != 0 || p.worldline != w.id)
                    _exit(13);
                const std::uint64_t out = h;
                if (::write(down[1], &out, sizeof(out)) != sizeof(out))
                    _exit(14);
            } else {
                ChannelCursor mc{10, 1}; // current generation, one metric already seen
                std::span<const std::byte> recs;
                bool snap = true;
                if (!channel_read(view, Channel::Metrics, &mc, &recs, &snap, nullptr))
                    _exit(15);
                if (snap || mc.count != 4)
                    _exit(16);
                const std::uint64_t out = static_cast<std::uint64_t>(recs.size());
                if (::write(down[1], &out, sizeof(out)) != sizeof(out))
                    _exit(17);
            }
        }
        _exit(0);
    }
    ::close(up[0]);
    ::close(down[1]);

    // Batch 1: one record per channel.
    check(write_metric(img, 100) == 4, "metrics generation advances");
    {
        std::uint64_t off = 0, gen = 0;
        check(channel_reserve(img, Channel::Cct, sizeof(CctRecord), &off, nullptr), "reserve cct");
        CctRecord c{};
        c.parent = kImageNoParent;
        c.mid_lo = 7;
        c.node = 0;
        c.samples = 1;
        c.self_ns = 10;
        put(img, off, c);
        check(channel_commit(img, Channel::Cct, off, sizeof(c), &gen, nullptr), "commit cct");

        check(channel_reserve(img, Channel::Patches, sizeof(PatchRecord), &off, nullptr),
              "reserve patch");
        PatchRecord p{};
        p.node = 0;
        p.field_offset = 8;
        p.new_value = 5;
        p.worldline = 1;
        p.generation = gen;
        put(img, off, p);
        check(channel_commit(img, Channel::Patches, off, sizeof(p), &gen, nullptr), "commit patch");

        check(channel_reserve(img, Channel::Worldlines, sizeof(WorldlineRecord), &off, nullptr),
              "reserve worldline");
        WorldlineRecord w{};
        w.id = 1;
        w.parent = kImageNoParent;
        w.state = static_cast<std::uint8_t>(WorldlineState::Kept);
        w.constraint_result = static_cast<std::uint8_t>(ConstraintResult::Pass);
        w.patch_off = 0;
        w.patch_len = sizeof(PatchRecord);
        put(img, off, w);
        check(channel_commit(img, Channel::Worldlines, off, sizeof(w), &gen, nullptr),
              "commit worldline");
    }
    check((load_header(img).committed_gen % 2) == 0, "committed generation stays even");
    {
        const auto vc = validate_image(img);
        if (!vc.ok())
            std::printf("after commits: %s (%s)\n",
                        std::string(image_status_name(vc.status)).c_str(), vc.detail);
        check(vc.ok(), "image still validates after commits");
    }

    char go = '1';
    check(::write(up[1], &go, 1) == 1, "signal phase 1");
    std::uint64_t child_sum = 0;
    check(::read(down[0], &child_sum, sizeof(child_sum)) == sizeof(child_sum), "child phase 1");
    {
        ChannelCursor mc, cc, pc, wc;
        std::span<const std::byte> mr, cr, pr, wr;
        bool snap = false;
        check(channel_read(img, Channel::Metrics, &mc, &mr, &snap, nullptr), "parent metrics");
        channel_read(img, Channel::Cct, &cc, &cr, &snap, nullptr);
        channel_read(img, Channel::Patches, &pc, &pr, &snap, nullptr);
        channel_read(img, Channel::Worldlines, &wc, &wr, &snap, nullptr);
        MetricRecord m = get<MetricRecord>(mr, 0);
        CctRecord c = get<CctRecord>(cr, 0);
        PatchRecord p = get<PatchRecord>(pr, 0);
        WorldlineRecord w = get<WorldlineRecord>(wr, 0);
        std::uint64_t h = 14695981039346656037ull;
        h = h * 131 + m.value;
        h = h * 131 + c.node;
        h = h * 131 + p.node;
        h = h * 131 + w.id;
        check(m.key_sym == 0 && c.node == p.node && p.worldline == w.id,
              "identity cross-reference");
        check(child_sum == h, "child agrees on batch 1");
    }

    // Batch 2: three more metrics; the child asks from generation 4.
    write_metric(img, 200);
    write_metric(img, 300);
    write_metric(img, 400);
    check(::write(up[1], &go, 1) == 1, "signal phase 2");
    std::uint64_t child_bytes = 0;
    check(::read(down[0], &child_bytes, sizeof(child_bytes)) == sizeof(child_bytes),
          "child phase 2");
    check(child_bytes == 3 * sizeof(MetricRecord), "child receives only the new records");

    // #5385: a cursor ahead of the section generation gets the full
    // snapshot, never a count-sliced range; gen 0 count 0 reads everything.
    {
        ChannelCursor stale{999, 1};
        std::span<const std::byte> recs;
        bool snap = false;
        check(channel_read(img, Channel::Metrics, &stale, &recs, &snap, nullptr), "ahead read");
        check(snap && recs.size() == 4 * sizeof(MetricRecord), "ahead generation rebuilds");
        ChannelCursor none;
        check(channel_read(img, Channel::Metrics, &none, &recs, &snap, nullptr), "no-cursor read");
        check(snap && recs.size() == 4 * sizeof(MetricRecord), "gen 0 count 0 is a full snapshot");
        ChannelCursor ahead{10, 99};
        check(channel_read(img, Channel::Metrics, &ahead, &recs, &snap, nullptr), "ahead read");
        check(snap, "count ahead of the channel rebuilds");
    }

    // Capacity: 8 metric slots total, 4 used; 4 fit, one more fails.
    for (int i = 0; i < 4; ++i)
        write_metric(img, 500 + static_cast<std::uint64_t>(i));
    {
        std::uint64_t offset = 0;
        std::string why;
        check(!channel_reserve(img, Channel::Metrics, sizeof(MetricRecord), &offset, &why),
              "capacity exhaustion is a hard error");
        check(why.find("capacity") != std::string::npos, "capacity reason");
    }

    int status = 0;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exits cleanly");
    check(digest_channels(img) != 0, "channel digest");
    ::close(up[1]);
    ::close(down[0]);
    munmap(map, kSize);
    ::close(fd);
    if (g_failed == 0)
        std::printf("OK: bus channels #5125\n");
    return g_failed == 0 ? 0 : 1;
}
