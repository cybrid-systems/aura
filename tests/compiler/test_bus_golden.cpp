// Issue #5122: committed golden bus image fixture.
//
// The fixture is checked in at tests/fixtures/bus/golden.img. Regenerate
// it with AURA_BUS_WRITE_GOLDEN=1 ./test_bus_golden after an intentional
// schema/layout change, then review the binary diff.
//
// Normal runs compare the canonical builder byte-for-byte with the
// fixture, validate it, read it from a second process through the same
// shared mapping, and prove a committed section stays immutable while
// the audit tail still appends.

#include "bus/image.hh"
#include "bus/image_schema.hh"
#include "bus/capability.hh"
#include "bus/query.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

template <typename T> void put(std::span<std::byte> img, std::uint64_t off, const T& v) {
    std::memcpy(img.data() + off, &v, sizeof(T));
}

std::uint64_t fnv(std::span<const std::byte> bytes) {
    std::uint64_t h = 14695981039346656037ull;
    for (const auto b : bytes) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(b));
        h *= 1099511628211ull;
    }
    return h;
}

template <typename T> std::span<const std::byte> as_bytes(const std::vector<T>& v) {
    return {reinterpret_cast<const std::byte*>(v.data()), v.size() * sizeof(T)};
}

std::filesystem::path fixture_path() {
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" / "bus" /
           "golden.img";
}

// Canonical image: five record sections plus an audit section with spare
// capacity (so the append test has room). Every field is deterministic.
std::vector<std::byte> build_golden() {
    using namespace aura::bus;
    constexpr std::uint32_t kSections = 6;
    constexpr std::uint64_t kDir = kImageHeaderSize;
    constexpr std::uint64_t kEntries = kSections * sizeof(SectionEntry);

    std::vector<NodeRecord> nodes(8);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        // NodeTag::Define is 8 in the frozen tag table; the fixture does
        // not depend on the module type.
        nodes[i].kind = i == 0 ? 8u : 2u;
        nodes[i].slot = i;
        nodes[i].parent = kImageNoIndex;
        nodes[i].first_child = kImageNoIndex;
        nodes[i].next_sibling = kImageNoIndex;
        nodes[i].sym = kImageNoIndex;
        nodes[i].src_line = static_cast<std::uint32_t>(10 + i);
        nodes[i].src_col = static_cast<std::uint32_t>(2 + i);
    }
    nodes[0].first_child = 1;
    nodes[1].next_sibling = 2;
    nodes[2].first_child = 3;

    std::vector<MetricRecord> metrics(4);
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        metrics[i].key_sym = kImageNoIndex;
        metrics[i].kind = 1;
        metrics[i].value = 100 + i;
    }

    std::vector<CctRecord> cct(4);
    for (std::size_t i = 0; i < cct.size(); ++i) {
        cct[i].parent = i == 0 ? kImageNoParent : i - 1;
        cct[i].mid_lo = 0x1100 + i;
        cct[i].node = kImageNoIndex;
        cct[i].samples = i + 1;
        cct[i].self_ns = 10 * (i + 1);
        cct[i].flags = 1;
    }

    std::vector<PatchRecord> patches(4);
    for (std::size_t i = 0; i < patches.size(); ++i) {
        patches[i].node = i == 0 ? 0 : (i <= 2 ? 1 : 2);
        patches[i].field_offset = static_cast<std::uint32_t>(i + 1);
        patches[i].new_value = i < 2 ? 1000 + i : 2000 + i;
        patches[i].worldline = kImageNoIndex;
        patches[i].generation = 2;
    }

    std::vector<WorldlineRecord> worldlines(4);
    for (std::size_t i = 0; i < worldlines.size(); ++i) {
        worldlines[i].id = i + 1;
        worldlines[i].parent = kImageNoParent;
        worldlines[i].state = static_cast<std::uint8_t>(WorldlineState::Kept);
        worldlines[i].constraint_result = static_cast<std::uint8_t>(ConstraintResult::Pass);
        worldlines[i].metrics_off = kImageNoIndex;
        if (i == 0) {
            worldlines[i].patch_off = 0;
            worldlines[i].patch_len = 2 * sizeof(PatchRecord);
            worldlines[i].patch_count = 2;
        } else if (i == 1) {
            worldlines[i].patch_off = 2 * sizeof(PatchRecord);
            worldlines[i].patch_len = 2 * sizeof(PatchRecord);
            worldlines[i].patch_count = 2;
        }
    }

    std::vector<AuditRecord> audit(8);
    for (std::size_t i = 0; i < audit.size(); ++i) {
        audit[i].seq = i + 1;
        audit[i].kind = static_cast<std::uint16_t>(AuditKind::Publish);
        audit[i].actor = 1;
        audit[i].value = i;
        audit[i].generation = 2;
    }

    auto align8 = [](std::uint64_t v) { return (v + 7) & ~7ull; };
    std::uint64_t off = kDir + kEntries;
    const std::uint64_t nodes_at = off;
    off = align8(off + as_bytes(nodes).size());
    const std::uint64_t metrics_at = off;
    off = align8(off + as_bytes(metrics).size());
    const std::uint64_t cct_at = off;
    off = align8(off + as_bytes(cct).size());
    const std::uint64_t patches_at = off;
    off = align8(off + as_bytes(patches).size());
    const std::uint64_t worldlines_at = off;
    off = align8(off + as_bytes(worldlines).size());
    const std::uint64_t audit_at = off;
    off = align8(off + 12 * sizeof(AuditRecord));
    const std::uint64_t committed = off;

    std::vector<std::byte> img(committed);
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
    h.section_dir_off = kDir;
    h.section_count = kSections;
    put(img, 0, h);

    auto entry = [&](std::uint32_t i, SectionKind kind, std::uint64_t at, std::uint64_t bytes,
                     std::uint64_t count, std::uint64_t capacity) {
        SectionEntry e{};
        e.kind = static_cast<std::uint32_t>(kind);
        e.version = 1;
        e.offset = at;
        e.element_size = static_cast<std::uint32_t>(bytes / count);
        e.capacity = capacity;
        e.count = static_cast<std::uint32_t>(count);
        e.length = bytes;
        e.generation = 2;
        e.fingerprint = section_layout_fingerprint(kind);
        e.digest = fnv({img.data() + at, static_cast<std::size_t>(bytes)});
        put(img, kDir + static_cast<std::uint64_t>(i) * sizeof(SectionEntry), e);
    };

    std::memcpy(img.data() + nodes_at, nodes.data(), as_bytes(nodes).size());
    std::memcpy(img.data() + metrics_at, metrics.data(), as_bytes(metrics).size());
    std::memcpy(img.data() + cct_at, cct.data(), as_bytes(cct).size());
    std::memcpy(img.data() + patches_at, patches.data(), as_bytes(patches).size());
    std::memcpy(img.data() + worldlines_at, worldlines.data(), as_bytes(worldlines).size());
    std::memcpy(img.data() + audit_at, audit.data(), as_bytes(audit).size());

    entry(0, SectionKind::Nodes, nodes_at, as_bytes(nodes).size(), nodes.size(),
          as_bytes(nodes).size());
    entry(1, SectionKind::Metrics, metrics_at, as_bytes(metrics).size(), metrics.size(),
          as_bytes(metrics).size());
    entry(2, SectionKind::Cct, cct_at, as_bytes(cct).size(), cct.size(), as_bytes(cct).size());
    entry(3, SectionKind::Patches, patches_at, as_bytes(patches).size(), patches.size(),
          as_bytes(patches).size());
    entry(4, SectionKind::Worldlines, worldlines_at, as_bytes(worldlines).size(), worldlines.size(),
          as_bytes(worldlines).size());
    entry(5, SectionKind::Audit, audit_at, as_bytes(audit).size(), audit.size(),
          12 * sizeof(AuditRecord));
    return img;
}

std::uint64_t section_digest(std::span<const std::byte> image, aura::bus::SectionKind kind) {
    using namespace aura::bus;
    std::uint64_t h = 14695981039346656037ull;
    const ImageStatus st =
        visit_section(image, kind, [&](const std::byte* rec, std::uint64_t element, std::uint64_t) {
            for (std::uint64_t i = 0; i < element; ++i) {
                h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(rec[i]));
                h *= 1099511628211ull;
            }
        });
    return st == ImageStatus::Ok ? h : 0;
}

bool write_fixture(std::span<const std::byte> bytes) {
    std::error_code ec;
    std::filesystem::create_directories(fixture_path().parent_path(), ec);
    std::ofstream out(fixture_path(), std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::vector<std::byte> read_fixture() {
    std::ifstream in(fixture_path(), std::ios::binary);
    if (!in)
        return {};
    const std::vector<char> raw{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
    std::vector<std::byte> out(raw.size());
    if (!raw.empty())
        std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

} // namespace

int main() {
    using namespace aura::bus;
    const std::vector<std::byte> golden = build_golden();
    check(validate_image(golden).ok(), "canonical image validates");

    if (const char* write = std::getenv("AURA_BUS_WRITE_GOLDEN"); write && write[0] == '1') {
        if (!write_fixture(golden)) {
            std::printf("FAIL: cannot write %s\n", fixture_path().c_str());
            return 1;
        }
        std::printf("wrote %s (%zu bytes, digest %016llx)\n", fixture_path().c_str(), golden.size(),
                    static_cast<unsigned long long>(committed_digest(golden)));
        return 0;
    }

    const std::vector<std::byte> fixture = read_fixture();
    check(fixture.size() == golden.size(), "fixture size matches the builder");
    if (fixture.size() == golden.size()) {
        bool same = true;
        for (std::size_t i = 0; i < fixture.size(); ++i) {
            if (fixture[i] != golden[i]) {
                std::printf("first byte diff at %zu: fixture %02x builder %02x\n", i,
                            static_cast<unsigned>(fixture[i]), static_cast<unsigned>(golden[i]));
                same = false;
                break;
            }
        }
        check(same, "fixture bytes match the builder");

        const std::span<const std::byte> view = fixture;
        check(validate_image(view).ok(), "fixture validates");
        const std::uint64_t expected = committed_digest(view);
        std::printf("golden digest %016llx\n", static_cast<unsigned long long>(expected));
        check(section_digest(view, SectionKind::Nodes) != 0, "node traversal");
        check(section_digest(view, SectionKind::Metrics) != 0, "metric traversal");
        check(section_digest(view, SectionKind::Cct) != 0, "CCT traversal");
        check(section_digest(view, SectionKind::Patches) != 0, "patch traversal");
        check(section_digest(view, SectionKind::Worldlines) != 0, "worldline traversal");
        check(audit_count(view) == 8, "audit count in the fixture");

        // Agent query face: CCT hotspots aggregate by mid, descending.
        {
            std::vector<Hotspot> hot;
            std::string why;
            check(cct_hotspots(view, 2, &hot, &why), "hotspots query");
            check(hot.size() == 2, "hotspots top-2");
            if (hot.size() == 2) {
                check(hot[0].mid == 0x1103 && hot[0].self_ns == 40, "hottest mid first");
                check(hot[1].mid == 0x1102 && hot[1].self_ns == 30, "second hottest mid");
            }
        }

        // Worldline diff: node 1 differs, nodes 0/2 are one-sided.
        {
            WorldlineDiff diff;
            std::string why;
            check(worldline_diff(view, 1, 2, &diff, &why), "worldline diff");
            check(diff.state_a == static_cast<std::uint8_t>(WorldlineState::Kept), "a kept");
            check(diff.rows.size() == 3, "three differing patch nodes");
            if (diff.rows.size() == 3) {
                check(diff.rows[0].node == 0 && diff.rows[0].in_a && !diff.rows[0].in_b,
                      "node 0 only in a");
                check(diff.rows[1].node == 1 && diff.rows[1].in_a && diff.rows[1].in_b &&
                          diff.rows[1].value_a != diff.rows[1].value_b,
                      "node 1 value differs");
                check(diff.rows[2].node == 2 && !diff.rows[2].in_a && diff.rows[2].in_b,
                      "node 2 only in b");
            }
        }

        // A second process reads the same mapping and agrees.
        int pipefd[2];
        check(::pipe(pipefd) == 0, "pipe");
        const int fd = static_cast<int>(::syscall(SYS_memfd_create, "bus-golden", 0));
        check(fd >= 0, "memfd");
        check(::ftruncate(fd, static_cast<off_t>(golden.size())) == 0, "ftruncate");
        check(::pwrite(fd, golden.data(), golden.size(), 0) == static_cast<ssize_t>(golden.size()),
              "seed memfd");
        const pid_t pid = ::fork();
        check(pid >= 0, "fork reader");
        if (pid == 0) {
            ::close(pipefd[0]);
            void* base = ::mmap(nullptr, golden.size(), PROT_READ, MAP_SHARED, fd, 0);
            if (base == MAP_FAILED)
                _exit(31);
            const std::span<const std::byte> child{static_cast<const std::byte*>(base),
                                                   golden.size()};
            std::uint64_t out[2] = {validate_image(child).ok() ? 1u : 0u, committed_digest(child)};
            if (::write(pipefd[1], out, sizeof(out)) != static_cast<ssize_t>(sizeof(out)))
                _exit(32);
            _exit(0);
        }
        ::close(pipefd[1]);
        std::uint64_t got[2] = {0, 0};
        check(::read(pipefd[0], got, sizeof(got)) == static_cast<ssize_t>(sizeof(got)),
              "read child digest");
        int status = 0;
        ::waitpid(pid, &status, 0);
        check(got[0] == 1, "child validates");
        check(got[1] == expected, "both processes read the same digest");
        ::close(pipefd[0]);
        ::close(fd);

        // A committed byte change is detected.
        std::vector<std::byte> corrupt(fixture.begin(), fixture.end());
        corrupt[300] = static_cast<std::byte>(static_cast<unsigned>(corrupt[300]) ^ 0x40u);
        check(!validate_image(corrupt).ok(), "in-place committed change rejected");

        // The audit tail can still append; committed channel data cannot.
        std::vector<std::byte> grown(fixture.begin(), fixture.end());
        AuditRecord extra{};
        extra.seq = 9;
        extra.kind = static_cast<std::uint16_t>(AuditKind::Keep);
        extra.actor = 1;
        extra.target = 2;
        extra.value = 7;
        std::string why;
        check(audit_append({grown.data(), grown.size()}, &extra, &why), "audit tail append");
        check(audit_count(grown) == 9, "audit count advanced");
        check(validate_image(grown).ok(), "image validates after audit append");
        AuditRecord again = extra;
        check(!audit_append({grown.data(), grown.size()}, &again, &why),
              "audit sequence must increase");
    }

    if (g_failed == 0)
        std::printf("OK: bus golden image #5122\n");
    return g_failed == 0 ? 0 : 1;
}
