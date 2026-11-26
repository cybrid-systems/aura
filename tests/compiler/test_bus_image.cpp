// Issue #5122: relocatable bus image — golden two-process traversal and
// untrusted-input rejection. No sockets and no capabilities here.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus/image.hh"
#include "bus/image_schema.hh"

// Count heap traffic so the golden traversal can prove it allocates
// nothing on the read path.
static std::size_t g_allocs = 0;

void* operator new(std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n))
        return p;
    std::abort();
}
void* operator new[](std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n))
        return p;
    std::abort();
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

using aura::bus::AuditRecord;
using aura::bus::CctRecord;
using aura::bus::ConstraintSetHeader;
using aura::bus::ImageHeader;
using aura::bus::ImageStatus;
using aura::bus::MetricRecord;
using aura::bus::NodeRecord;
using aura::bus::PatchRecord;
using aura::bus::SectionEntry;
using aura::bus::SectionKind;
using aura::bus::StringTableEntry;
using aura::bus::SymbolEntry;
using aura::bus::WorldlineRecord;

constexpr std::uint64_t align8(std::uint64_t v) {
    return (v + 7) & ~7ull;
}

std::uint64_t fnv_bytes(std::span<const std::byte> img, std::uint64_t off, std::uint64_t len) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::uint64_t i = 0; i < len; ++i) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(img[off + i]));
        hash *= 1099511628211ull;
    }
    return hash;
}

template <typename T> void put(std::span<std::byte> img, std::uint64_t off, const T& v) {
    std::memcpy(img.data() + off, &v, sizeof(T));
}

template <typename T> T get(std::span<const std::byte> img, std::uint64_t off) {
    T v{};
    std::memcpy(&v, img.data() + off, sizeof(T));
    return v;
}

struct Built {
    std::uint64_t committed_bytes = 0;
    std::uint64_t nodes_off = 0;
    std::uint64_t patches_off = 0;
    std::uint64_t metrics_off = 0;
    std::uint64_t cct_off = 0;
    std::uint64_t worldlines_off = 0;
    std::uint64_t audit_off = 0;
    std::uint64_t symbols_off = 0;
};

// node_tail_bytes models a later layout revision: each node record grows
// by that many zero bytes while the frozen prefix (and schema id) stays.
Built build_image(std::span<std::byte> img, std::uint64_t node_tail_bytes = 0) {
    Built b{};
    const std::uint32_t section_count = 10;
    constexpr std::uint64_t dir_slots = 11; // one spare slot for the unknown-section case
    const std::uint64_t dir_off = 256;
    std::uint64_t off = dir_off + dir_slots * sizeof(SectionEntry);

    const std::uint64_t strings_off = off;
    // #5378: the string table reserves a fixed entry array; the string
    // bytes follow it, so an entry's offset is always past the array.
    const std::uint64_t strings_reserved =
        (aura::bus::kStringSlotsReserved * sizeof(StringTableEntry) + 7) & ~7ull;
    const std::uint64_t strings_len = strings_reserved + 64;
    off = align8(off + strings_len);

    const std::uint64_t symbols_off = off;
    const std::uint64_t symbols_len = 2 * sizeof(SymbolEntry);
    off = align8(off + symbols_len);

    const std::uint64_t node_stride = sizeof(NodeRecord) + node_tail_bytes;
    const std::uint64_t nodes_off = off;
    const std::uint64_t nodes_len = 3 * node_stride;
    off = align8(off + nodes_len);

    const std::uint64_t patches_off = off;
    const std::uint64_t patches_len = sizeof(PatchRecord);
    off = align8(off + patches_len);

    const std::uint64_t metrics_off = off;
    const std::uint64_t metrics_len = 2 * sizeof(MetricRecord);
    off = align8(off + metrics_len);

    const std::uint64_t cct_off = off;
    const std::uint64_t cct_len = 3 * sizeof(CctRecord);
    off = align8(off + cct_len);

    const std::uint64_t worldlines_off = off;
    const std::uint64_t worldlines_len = 2 * sizeof(WorldlineRecord);
    off = align8(off + worldlines_len);

    const std::uint64_t constraints_off = off;
    const std::uint64_t constraints_len = sizeof(ConstraintSetHeader) + 8;
    off = align8(off + constraints_len);

    const std::uint64_t audit_off = off;
    const std::uint64_t audit_len = 2 * sizeof(AuditRecord);
    off = align8(off + audit_len);

    const std::uint64_t staging_off = off;
    const std::uint64_t committed = align8(off);
    b.committed_bytes = committed;
    b.nodes_off = nodes_off;
    b.patches_off = patches_off;
    b.metrics_off = metrics_off;
    b.cct_off = cct_off;
    b.worldlines_off = worldlines_off;
    b.audit_off = audit_off;
    b.symbols_off = symbols_off;

    std::memset(img.data(), 0, committed);
    ImageHeader h{};
    std::memcpy(h.magic, aura::bus::kImageMagic, sizeof(h.magic));
    h.abi_major = aura::bus::kImageAbiMajor;
    h.abi_minor = aura::bus::kImageAbiMinor;
    h.endianness = static_cast<std::uint32_t>(aura::bus::kImageEndianMarker);
    h.header_len = aura::bus::kImageHeaderSize;
    h.flags = 0;
    h.image_size = img.size();
    const auto id = aura::bus::header_schema_id();
    std::memcpy(h.schema_id, &id.a, 8);
    std::memcpy(h.schema_id + 8, &id.b, 8);
    h.layout_version = node_tail_bytes == 0 ? 1 : 2;
    h.committed_bytes = committed;
    h.committed_gen = 2;
    h.bell = 0;
    h.section_dir_off = dir_off;
    h.section_count = section_count;
    put(img, 0, h);

    auto entry = [&](std::uint32_t index, SectionKind kind, std::uint64_t o, std::uint64_t len,
                     std::uint32_t elem, std::uint32_t count) {
        SectionEntry e{};
        e.kind = static_cast<std::uint32_t>(kind);
        e.version = 1;
        e.flags = 0;
        e.offset = o;
        e.length = len;
        e.element_size = elem;
        e.count = count;
        e.capacity = len;
        e.generation = 2;
        e.fingerprint = aura::bus::section_layout_fingerprint(kind);
        put(img, dir_off + static_cast<std::uint64_t>(index) * sizeof(SectionEntry), e);
    };
    entry(0, SectionKind::StringTable, strings_off, strings_len, sizeof(StringTableEntry), 2);
    entry(1, SectionKind::SymbolTable, symbols_off, symbols_len, sizeof(SymbolEntry), 2);
    entry(2, SectionKind::Nodes, nodes_off, nodes_len, static_cast<std::uint32_t>(node_stride), 3);
    entry(3, SectionKind::Patches, patches_off, patches_len, sizeof(PatchRecord), 1);
    entry(4, SectionKind::Metrics, metrics_off, metrics_len, sizeof(MetricRecord), 2);
    entry(5, SectionKind::Cct, cct_off, cct_len, sizeof(CctRecord), 3);
    entry(6, SectionKind::Worldlines, worldlines_off, worldlines_len, sizeof(WorldlineRecord), 2);
    entry(7, SectionKind::Constraints, constraints_off, constraints_len,
          sizeof(ConstraintSetHeader), 0);
    entry(8, SectionKind::Audit, audit_off, audit_len, sizeof(AuditRecord), 2);
    entry(9, SectionKind::Staging, staging_off, 0, 1, 0);

    // String table: two 8-aligned slices after the reserved entry array.
    put(img, strings_off, StringTableEntry{strings_reserved, 5, 0});
    put(img, strings_off + sizeof(StringTableEntry), StringTableEntry{strings_reserved + 8, 5, 0});
    std::memcpy(img.data() + strings_off + strings_reserved, "hello", 5);
    std::memcpy(img.data() + strings_off + strings_reserved + 8, "world", 5);

    put(img, symbols_off, SymbolEntry{strings_reserved, 5, 0});
    put(img, symbols_off + sizeof(SymbolEntry), SymbolEntry{strings_reserved + 8, 5, 0});

    NodeRecord n0{};
    n0.kind = 1;
    n0.slot = 0;
    n0.parent = aura::bus::kImageNoIndex;
    n0.first_child = 1;
    n0.next_sibling = aura::bus::kImageNoIndex;
    n0.sym = 0;
    n0.src_line = 1;
    n0.src_col = 1;
    put(img, nodes_off, n0);
    NodeRecord n1{};
    n1.kind = 2;
    n1.slot = 1;
    n1.parent = 0;
    n1.first_child = aura::bus::kImageNoIndex;
    n1.next_sibling = 2;
    n1.sym = 1;
    n1.src_line = 2;
    put(img, nodes_off + node_stride, n1);
    NodeRecord n2{};
    n2.kind = 3;
    n2.slot = 2;
    n2.parent = 0;
    n2.first_child = aura::bus::kImageNoIndex;
    n2.next_sibling = aura::bus::kImageNoIndex;
    n2.sym = aura::bus::kImageNoIndex;
    n2.src_line = 3;
    put(img, nodes_off + 2 * node_stride, n2);

    PatchRecord p{};
    p.node = 1;
    p.field_offset = 8;
    p.new_value = 42;
    p.worldline = 1;
    p.generation = 2;
    put(img, patches_off, p);

    MetricRecord m0{};
    m0.key_sym = 0;
    m0.kind = 1;
    m0.value = 100;
    put(img, metrics_off, m0);
    MetricRecord m1{};
    m1.key_sym = 1;
    m1.kind = 2;
    m1.value = 200;
    put(img, metrics_off + sizeof(MetricRecord), m1);

    CctRecord c0{};
    c0.parent = aura::bus::kImageNoParent;
    c0.mid_lo = 0x1234;
    c0.node = 0;
    c0.samples = 10;
    c0.self_ns = 1000;
    put(img, cct_off, c0);
    CctRecord c1{};
    c1.parent = 0;
    c1.mid_lo = 0x2345;
    c1.node = 1;
    c1.samples = 20;
    c1.self_ns = 2000;
    put(img, cct_off + sizeof(CctRecord), c1);
    CctRecord c2{};
    c2.parent = 1;
    c2.mid_lo = 0x3456;
    c2.node = 2;
    c2.samples = 30;
    c2.self_ns = 3000;
    put(img, cct_off + 2 * sizeof(CctRecord), c2);

    WorldlineRecord w0{};
    w0.id = 1;
    w0.parent = aura::bus::kImageNoParent;
    w0.state = static_cast<std::uint8_t>(aura::bus::WorldlineState::Kept);
    w0.constraint_result = static_cast<std::uint8_t>(aura::bus::ConstraintResult::Pass);
    w0.patch_count = 1;
    w0.metrics_off = 0;
    w0.patch_off = 0;
    w0.patch_len = sizeof(PatchRecord);
    put(img, worldlines_off, w0);
    WorldlineRecord w1{};
    w1.id = 2;
    w1.parent = 0;
    w1.state = static_cast<std::uint8_t>(aura::bus::WorldlineState::Pending);
    w1.constraint_result = static_cast<std::uint8_t>(aura::bus::ConstraintResult::None);
    w1.metrics_off = sizeof(MetricRecord);
    put(img, worldlines_off + sizeof(WorldlineRecord), w1);

    ConstraintSetHeader cs{};
    cs.version = 1;
    cs.object_off = sizeof(ConstraintSetHeader);
    cs.object_len = 8;
    cs.schema = 0xabc;
    put(img, constraints_off, cs);
    std::memcpy(img.data() + constraints_off + sizeof(ConstraintSetHeader), "guard!!!", 8);

    AuditRecord a0{};
    a0.seq = 1;
    a0.kind = 1;
    a0.target = 1;
    a0.generation = 2;
    put(img, audit_off, a0);
    AuditRecord a1{};
    a1.seq = 2;
    a1.kind = 2;
    a1.target = 2;
    a1.generation = 2;
    put(img, audit_off + sizeof(AuditRecord), a1);

    // Commit: every known section carries the digest of its bytes; the
    // string table's chains its entries.
    for (std::uint32_t i = 0; i < section_count; ++i) {
        const std::uint64_t at = dir_off + static_cast<std::uint64_t>(i) * sizeof(SectionEntry);
        SectionEntry e = get<SectionEntry>(img, at);
        if (e.kind == static_cast<std::uint32_t>(SectionKind::Staging))
            continue;
        e.digest = e.kind == static_cast<std::uint32_t>(SectionKind::StringTable)
                       ? aura::bus::string_table_digest(img, e, 0, 14695981039346656037ull)
                       : fnv_bytes(img, e.offset, e.length);
        put(img, at, e);
    }

    return b;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

// Structural digest over the committed records. The walk allocates
// nothing; it reads through visit_section.
std::uint64_t structural_digest(std::span<const std::byte> img) {
    std::uint64_t h = 14695981039346656037ull;
    auto rc = aura::bus::visit_section(
        img, SectionKind::Nodes, [&](const std::byte* rec, std::uint64_t elem, std::uint64_t) {
            NodeRecord n{};
            if (!aura::bus::read_record(rec, elem, &n))
                return;
            h = mix(h, n.kind);
            h = mix(h, n.slot);
            h = mix(h, n.parent);
            h = mix(h, n.first_child);
            h = mix(h, n.next_sibling);
            h = mix(h, n.sym);
        });
    check(rc == ImageStatus::Ok, "walk nodes");
    rc = aura::bus::visit_section(img, SectionKind::Patches,
                                  [&](const std::byte* rec, std::uint64_t elem, std::uint64_t) {
                                      PatchRecord p{};
                                      if (!aura::bus::read_record(rec, elem, &p))
                                          return;
                                      h = mix(h, p.node);
                                      h = mix(h, p.new_value);
                                      h = mix(h, p.worldline);
                                  });
    check(rc == ImageStatus::Ok, "walk patches");
    rc = aura::bus::visit_section(img, SectionKind::Metrics,
                                  [&](const std::byte* rec, std::uint64_t elem, std::uint64_t) {
                                      MetricRecord m{};
                                      if (!aura::bus::read_record(rec, elem, &m))
                                          return;
                                      h = mix(h, m.key_sym);
                                      h = mix(h, m.value);
                                  });
    check(rc == ImageStatus::Ok, "walk metrics");
    rc = aura::bus::visit_section(img, SectionKind::Cct,
                                  [&](const std::byte* rec, std::uint64_t elem, std::uint64_t) {
                                      CctRecord c{};
                                      if (!aura::bus::read_record(rec, elem, &c))
                                          return;
                                      h = mix(h, c.parent);
                                      h = mix(h, c.mid_lo);
                                      h = mix(h, c.self_ns);
                                  });
    check(rc == ImageStatus::Ok, "walk cct");
    rc = aura::bus::visit_section(img, SectionKind::Worldlines,
                                  [&](const std::byte* rec, std::uint64_t elem, std::uint64_t) {
                                      WorldlineRecord w{};
                                      if (!aura::bus::read_record(rec, elem, &w))
                                          return;
                                      h = mix(h, w.id);
                                      h = mix(h, w.state);
                                      h = mix(h, w.patch_len);
                                  });
    check(rc == ImageStatus::Ok, "walk worldlines");
    return h;
}

std::uint64_t section_dir_off_of(std::span<const std::byte> img, SectionKind kind) {
    const ImageHeader h = aura::bus::load_header(img);
    for (std::uint32_t i = 0; i < h.section_count; ++i) {
        const std::uint64_t at = h.section_dir_off + i * sizeof(SectionEntry);
        const SectionEntry e = get<SectionEntry>(img, at);
        if (e.kind == static_cast<std::uint32_t>(kind))
            return at;
    }
    return 0;
}

} // namespace

int main() {
    constexpr std::uint64_t kSize = 1u << 16;
    std::vector<std::byte> backing(kSize);
    std::span<std::byte> img(backing);
    build_image(img);

    {
        const auto vc = aura::bus::validate_image(img);
        if (!vc.ok())
            std::printf("golden image: %s (%s)\n",
                        std::string(aura::bus::image_status_name(vc.status)).c_str(), vc.detail);
        check(vc.ok(), "golden image validates");
    }
    const std::uint64_t golden = structural_digest(img);
    {
        const std::size_t before = g_allocs;
        const std::uint64_t again = structural_digest(img);
        check(again == golden, "structural digest is stable");
        check(g_allocs == before, "traversal allocates nothing");
    }

    // ── Golden two-process read ──────────────────────────────────
    {
        const int fd = static_cast<int>(syscall(SYS_memfd_create, "aura-bus-image", 1));
        check(fd >= 0, "memfd_create");
        if (fd >= 0) {
            check(ftruncate(fd, kSize) == 0, "ftruncate");
            void* map = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            check(map != MAP_FAILED, "parent mmap");
            if (map != MAP_FAILED) {
                std::memset(map, 0, kSize);
                build_image(std::span<std::byte>(static_cast<std::byte*>(map), kSize));
                check(aura::bus::validate_image(
                          std::span<const std::byte>(static_cast<std::byte*>(map), kSize))
                          .ok(),
                      "shared image validates");

                int pipefd[2];
                check(pipe(pipefd) == 0, "pipe");
                const pid_t pid = fork();
                check(pid >= 0, "fork");
                if (pid == 0) {
                    close(pipefd[0]);
                    void* ro = mmap(nullptr, kSize, PROT_READ, MAP_SHARED, fd, 0);
                    if (ro == MAP_FAILED)
                        _exit(10);
                    const auto view =
                        std::span<const std::byte>(static_cast<const std::byte*>(ro), kSize);
                    const auto vc = aura::bus::validate_image(view);
                    if (!vc.ok())
                        _exit(11);
                    const std::uint64_t sum = structural_digest(view);
                    const std::uint64_t dg = aura::bus::committed_digest(view);
                    const std::uint64_t out[2] = {sum, dg};
                    const ssize_t w = write(pipefd[1], out, sizeof(out));
                    _exit(w == static_cast<ssize_t>(sizeof(out)) ? 0 : 12);
                }
                close(pipefd[1]);
                std::uint64_t child[2] = {0, 0};
                const ssize_t got = read(pipefd[0], child, sizeof(child));
                int status = 0;
                waitpid(pid, &status, 0);
                close(pipefd[0]);
                check(got == static_cast<ssize_t>(sizeof(child)), "child result read");
                check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child clean exit");
                check(child[0] == golden, "child nodes, patches, metrics and CCT agree");
                check(child[1] == aura::bus::committed_digest(std::span<const std::byte>(
                                      static_cast<const std::byte*>(map), kSize)),
                      "child committed digest agrees");
                munmap(map, kSize);
            }
            close(fd);
        }
    }

    // ── Corruptions must fail with a specific status ─────────────
    auto copy = backing;
    const std::span<const std::byte> base(copy);

    auto expect = [&](ImageStatus want, const char* label) {
        const auto vc = aura::bus::validate_image(std::span<const std::byte>(copy));
        if (vc.status != want) {
            ++g_failed;
            std::printf("FAIL: %s (got %s)\n", label,
                        std::string(aura::bus::image_status_name(vc.status)).c_str());
        }
    };

    copy = backing;
    copy[0] = std::byte{'X'};
    expect(ImageStatus::BadMagic, "bad magic");

    copy = backing;
    put(copy, offsetof(ImageHeader, endianness), std::uint32_t{0x04030201});
    expect(ImageStatus::BadEndian, "bad endian");

    copy = backing;
    put(copy, offsetof(ImageHeader, header_len), std::uint32_t{128});
    expect(ImageStatus::BadHeader, "bad header length");

    copy = backing;
    put(copy, offsetof(ImageHeader, image_size), std::uint64_t{123});
    expect(ImageStatus::BadSize, "sealed size mismatch");

    copy = backing;
    copy[offsetof(ImageHeader, schema_id)] = std::byte{0xFF};
    expect(ImageStatus::BadSchema, "bad schema id");

    copy = backing;
    put(copy, offsetof(ImageHeader, committed_gen), std::uint64_t{3});
    expect(ImageStatus::BadGeneration, "odd committed generation");

    copy = backing;
    put(copy, offsetof(ImageHeader, section_dir_off), std::uint64_t{260});
    expect(ImageStatus::BadDirectory, "misaligned directory");

    copy = backing;
    // Strings section offset misaligned.
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::StringTable);
        put(copy, at + offsetof(SectionEntry, offset), std::uint64_t{657});
    }
    expect(ImageStatus::BadAlignment, "misaligned section");

    copy = backing;
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::Nodes);
        put(copy, at + offsetof(SectionEntry, length), std::uint64_t{1u << 30});
    }
    expect(ImageStatus::BadBounds, "section outside committed prefix");

    copy = backing;
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::Nodes);
        put(copy, at + offsetof(SectionEntry, element_size), std::uint32_t{8});
    }
    expect(ImageStatus::BadElementSize, "narrow element size");

    copy = backing;
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::Nodes);
        put(copy, at + offsetof(SectionEntry, fingerprint), std::uint64_t{1});
    }
    expect(ImageStatus::BadFingerprint, "fingerprint mismatch");

    copy = backing;
    {
        const ImageHeader h = aura::bus::load_header(copy);
        (void)h;
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::Patches);
        const SectionEntry e = get<SectionEntry>(copy, at);
        put(copy, e.offset + offsetof(PatchRecord, new_value), std::uint64_t{1} << 63);
    }
    expect(ImageStatus::BadImmediate, "patch value bit 63");

    copy = backing;
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::Metrics);
        const SectionEntry e = get<SectionEntry>(copy, at);
        put(copy, e.offset + offsetof(MetricRecord, value), std::uint64_t{1} << 63);
    }
    expect(ImageStatus::BadImmediate, "metric value bit 63");

    copy = backing;
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::Cct);
        const SectionEntry e = get<SectionEntry>(copy, at);
        put(copy, e.offset + std::uint64_t{1} * e.element_size + offsetof(CctRecord, mid_lo),
            std::uint64_t{1} << 62);
    }
    expect(ImageStatus::BadImmediate, "CCT mid above the low 62");

    copy = backing;
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::Nodes);
        const SectionEntry e = get<SectionEntry>(copy, at);
        put(copy, e.offset + offsetof(NodeRecord, parent), std::uint64_t{99});
    }
    expect(ImageStatus::BadIndex, "node edge out of range");

    copy = backing;
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::SymbolTable);
        SectionEntry e = get<SectionEntry>(copy, at);
        const std::uint64_t sat = section_dir_off_of(copy, SectionKind::StringTable);
        const SectionEntry se = get<SectionEntry>(copy, sat);
        // A name past the end of the string section.
        put(copy, e.offset + offsetof(SymbolEntry, name_off), se.length + 4096);
        // Re-commit the section digest: the out-of-range name is the
        // defect under test, not a stale digest.
        e.digest = fnv_bytes(copy, e.offset, e.length);
        put(copy, at, e);
    }
    expect(ImageStatus::BadIndex, "symbol name outside the string table");

    copy = backing;
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::Cct);
        const SectionEntry e = get<SectionEntry>(copy, at);
        put(copy, e.offset + std::uint64_t{1} * e.element_size + offsetof(CctRecord, parent),
            std::uint64_t{1});
    }
    expect(ImageStatus::BadCycle, "CCT self parent");

    copy = backing;
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::Worldlines);
        const SectionEntry e = get<SectionEntry>(copy, at);
        put(copy, e.offset + offsetof(WorldlineRecord, state), std::uint8_t{9});
    }
    expect(ImageStatus::BadSection, "worldline state out of domain");

    copy = backing;
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::Audit);
        const SectionEntry e = get<SectionEntry>(copy, at);
        put(copy, e.offset + std::uint64_t{1} * e.element_size + offsetof(AuditRecord, seq),
            std::uint64_t{0});
    }
    expect(ImageStatus::BadSection, "audit sequence goes backwards");

    copy = backing;
    {
        const std::uint64_t at = section_dir_off_of(copy, SectionKind::Nodes);
        const SectionEntry e = get<SectionEntry>(copy, at);
        put(copy, e.offset + offsetof(NodeRecord, kind), std::uint32_t{9});
    }
    expect(ImageStatus::BadDigest, "in-place change to a committed node");

    {
        const std::span<const std::byte> truncated(base.data(), 100);
        const auto vc = aura::bus::validate_image(truncated);
        check(vc.status == ImageStatus::Truncated, "truncated image rejected");
    }

    // ── Additive compatibility: tail fields and unknown sections ─
    {
        std::vector<std::byte> grown(1u << 16);
        std::span<std::byte> g(grown);
        const Built b = build_image(g, 8);
        const auto vc = aura::bus::validate_image(g);
        if (!vc.ok())
            std::printf("FAIL: tail-field image (%s)\n",
                        std::string(aura::bus::image_status_name(vc.status)).c_str());
        check(vc.ok(), "appended node tail fields accepted");
        check(structural_digest(g) == golden, "tail fields do not change frozen reads");
        check(b.nodes_off != 0, "tail build offsets");

        // Unknown section kind: ignored by this reader.
        SectionEntry unknown{};
        unknown.kind = 99;
        unknown.version = 1;
        unknown.offset = align8(b.audit_off + 2 * sizeof(AuditRecord));
        unknown.length = 8;
        unknown.element_size = 1;
        unknown.capacity = 8;
        unknown.fingerprint = 0;
        const ImageHeader h = aura::bus::load_header(g);
        put(g,
            h.section_dir_off + static_cast<std::uint64_t>(h.section_count) * sizeof(SectionEntry),
            unknown);
        ImageHeader h2 = h;
        h2.section_count = h.section_count + 1;
        h2.committed_bytes = align8(unknown.offset + unknown.length);
        h2.image_size = g.size();
        put(g, 0, h2);
        const auto vc2 = aura::bus::validate_image(g);
        check(vc2.ok(), "unknown section kind is ignored");
    }

    // ── The string table chains its appends ─────────────────────
    // An append adds its entry and its text to the committed digest. A
    // reader that already verified the table replays only what was
    // appended, and a changed entry or text is still a mismatch.
    {
        // An image that holds a string table alone: the entry array of
        // the first layout, then room for 32 sources of eight bytes.
        constexpr std::uint64_t kEntries =
            aura::bus::kStringSlotsReserved * sizeof(StringTableEntry);
        constexpr std::uint64_t kTableOff = aura::bus::kImageHeaderSize + sizeof(SectionEntry);
        constexpr std::uint64_t kTableBytes = kEntries + 256;
        std::vector<std::byte> table_image(kTableOff + kTableBytes, std::byte{0});
        const auto view = [&] { return std::span<std::byte>(table_image); };
        const auto table = [&] { return get<SectionEntry>(view(), aura::bus::kImageHeaderSize); };
        {
            ImageHeader th{};
            std::memcpy(th.magic, aura::bus::kImageMagic, sizeof(th.magic));
            th.abi_major = aura::bus::kImageAbiMajor;
            th.abi_minor = aura::bus::kImageAbiMinor;
            th.endianness = aura::bus::kImageEndianMarker;
            th.header_len = aura::bus::kImageHeaderSize;
            th.image_size = table_image.size();
            const aura::bus::SchemaId sid = aura::bus::header_schema_id();
            std::memcpy(th.schema_id, &sid.a, 8);
            std::memcpy(th.schema_id + 8, &sid.b, 8);
            th.layout_version = 1;
            th.committed_bytes = table_image.size();
            th.committed_gen = 2;
            th.section_dir_off = aura::bus::kImageHeaderSize;
            th.section_count = 1;
            put(view(), 0, th);
            SectionEntry te{};
            te.kind = static_cast<std::uint32_t>(SectionKind::StringTable);
            te.version = 1;
            te.offset = kTableOff;
            te.length = kEntries;
            te.element_size = sizeof(StringTableEntry);
            te.capacity = kTableBytes;
            te.generation = 2;
            te.fingerprint = aura::bus::section_layout_fingerprint(SectionKind::StringTable);
            te.digest = 14695981039346656037ull;
            put(view(), aura::bus::kImageHeaderSize, te);
        }
        std::string why;
        std::uint64_t index = 0;
        aura::bus::ImageDigestCache cache{};
        check(aura::bus::validate_image(view(), &cache).ok(), "the empty table validates");
        for (int i = 0; i < 32; ++i) {
            check(aura::bus::string_table_append(view(), "more", &index, &why),
                  "a source is appended");
            check(index == static_cast<std::uint64_t>(i), "the source takes the next slot");
            check(aura::bus::validate_image(view(), &cache).ok(),
                  "the appended table validates on the cached prefix");
            check(aura::bus::validate_image(view()).ok(), "the appended table validates cold");
        }
        check(!aura::bus::string_table_append(view(), "more", &index, &why) &&
                  why.rfind("string table bytes full", 0) == 0,
              "the byte region bounds the table");
        std::string_view text;
        check(aura::bus::string_table_read(view(), 31, &text) && text == "more",
              "the appended source reads back");
        check(aura::bus::string_table_digest(view(), table(), 0, 14695981039346656037ull) ==
                  table().digest,
              "the committed digest is the chain of the entries");
        // The last byte the section committed is the padding of the
        // last source.
        table_image.back() = std::byte{'M'};
        check(aura::bus::validate_image(view()).status == ImageStatus::BadDigest,
              "in-place change to an appended source");
        table_image.back() = std::byte{0};
        put(view(), kTableOff + offsetof(StringTableEntry, len), std::uint32_t{3});
        check(aura::bus::validate_image(view()).status == ImageStatus::BadDigest,
              "in-place change to a string entry");
    }

    if (g_failed == 0)
        std::printf("OK: bus image #5122\n");
    return g_failed == 0 ? 0 : 1;
}
