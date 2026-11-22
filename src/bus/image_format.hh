// ──────────────────────────────────────────────────────────────
//  image_format.hh — v0.3.0 relocatable image protocol (#5122)
//
//  Wire layout only: no reflection, no allocation, no host ABI.
//  Every offset is relative to the image base; every integer is
//  little-endian; every record is fixed width. The compiler's C++
//  object ABI is not the protocol.
//
//  Header layout (256 bytes):
//    0 magic "AURAIMG\n", 8 abi_major/abi_minor, 12 endianness,
//    16 header_len, 20 flags, 24 image_size, 32 schema_id[16],
//    48 layout_version, 56 committed_bytes, 64 committed_gen
//    (own cache line), 128 bell (own cache line), 192 section dir.
//
//  The two hot words are isolated on their own cache lines; the
//  section directory follows them so a reader's version checks do
//  not share a line with the writer's generation store.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_IMAGE_FORMAT_HH
#define AURA_BUS_IMAGE_FORMAT_HH

#include <cstddef>
#include <cstdint>

namespace aura::bus {

inline constexpr std::uint32_t kImageAbiMajor = 1;
inline constexpr std::uint32_t kImageAbiMinor = 0;
inline constexpr std::uint32_t kImageHeaderSize = 256;
inline constexpr std::uint64_t kImageEndianMarker = 0x01020304ull;
inline constexpr std::uint64_t kImageNoParent = ~0ull;
inline constexpr std::uint64_t kImageNoIndex = ~0ull;
inline constexpr std::uint32_t kImageMaxRecords = 1u << 24;
inline constexpr std::uint64_t kImageMaxTotalRecords = 1ull << 26;
inline constexpr char kImageMagic[8] = {'A', 'U', 'R', 'A', 'I', 'M', 'G', '\n'};

enum class SectionKind : std::uint32_t {
    StringTable = 1,
    SymbolTable = 2,
    Nodes = 3,
    Patches = 4,
    Metrics = 5,
    Cct = 6,
    Worldlines = 7,
    Constraints = 8,
    Audit = 9,
    Staging = 10,
};

struct ImageHeader {
    char magic[8];
    std::uint16_t abi_major;
    std::uint16_t abi_minor;
    std::uint32_t endianness;
    std::uint32_t header_len;
    std::uint32_t flags;
    std::uint64_t image_size;
    std::uint8_t schema_id[16];
    std::uint64_t layout_version;
    std::uint64_t committed_bytes;
    std::uint64_t committed_gen; // even = stable; own cache line
    std::uint8_t pad0[56];
    std::uint64_t bell; // eventfd value / futex word; own cache line
    std::uint8_t pad1[56];
    std::uint64_t section_dir_off;
    std::uint32_t section_count;
    std::uint32_t reserved0;
    std::uint8_t reserved1[48];
};

struct SectionEntry {
    std::uint32_t kind;
    std::uint16_t version;
    std::uint16_t flags;
    std::uint64_t offset;
    std::uint64_t length;
    std::uint32_t element_size;
    std::uint32_t count;       // records, or string-table entries
    std::uint64_t capacity;    // reserved bytes; committed length <= capacity
    std::uint64_t fingerprint; // layout/schema fingerprint
    std::uint64_t digest;      // FNV-1a of the committed section bytes
    std::uint64_t generation;  // last committed generation for this section
};

struct StringTableEntry {
    std::uint64_t off;
    std::uint32_t len;
    std::uint32_t reserved;
};

// SymId is the entry index in the symbol table; the entry names a
// string-table slice. Session-stable, never a process pointer.
struct SymbolEntry {
    std::uint64_t name_off;
    std::uint32_t name_len;
    std::uint32_t flags;
};

// FlatAST SoA algebra: kind, slot, source, flags, index edges.
struct NodeRecord {
    std::uint32_t kind;
    std::uint16_t flags;
    std::uint16_t reserved0;
    std::uint64_t slot;
    std::uint64_t parent;
    std::uint64_t first_child;
    std::uint64_t next_sibling;
    std::uint64_t sym;
    std::uint32_t src_file;
    std::uint32_t src_line;
    std::uint32_t src_col;
    std::uint32_t reserved1;
};

struct PatchRecord {
    std::uint64_t node;
    std::uint32_t field_offset;
    std::uint32_t reserved;
    std::uint64_t new_value; // immediate: bit 63 must be 0
    std::uint64_t worldline;
    std::uint64_t generation;
};

struct MetricRecord {
    std::uint64_t key_sym;
    std::uint8_t kind;
    std::uint8_t flags;
    std::uint16_t reserved;
    std::uint32_t reserved2;
    std::uint64_t value; // immediate: bit 63 must be 0
};

struct CctRecord {
    std::uint64_t parent; // index or kImageNoParent
    std::uint64_t mid_lo; // low 62 bits only
    std::uint64_t node;   // NodeRecord index or kImageNoIndex
    std::uint64_t samples;
    std::uint64_t self_ns;
    std::uint32_t flags;
    std::uint32_t reserved;
};

enum class WorldlineState : std::uint8_t {
    Pending = 0,
    Kept = 1,
    Rolled = 2,
    Rejected = 3,
    // #5381: applied and under online observation; seals to Kept or
    // reverts to Rolled. Additive enum value (#5122 tail-compat rule).
    Canary = 4,
};

enum class ConstraintResult : std::uint8_t {
    None = 0,
    Pass = 1,
    Fail = 2,
};

struct WorldlineRecord {
    std::uint64_t id;
    std::uint64_t parent; // index or kImageNoParent
    std::uint8_t state;
    std::uint8_t constraint_result;
    std::uint16_t reserved;
    std::uint32_t patch_count;
    std::uint64_t metrics_off;
    std::uint64_t patch_off;
    std::uint64_t patch_len;
    // ── #5379 evidence tail (layout_version 2) ─────────────────
    // The frozen prefix above keeps the v1 schema id; a v1 reader
    // skips this tail. Zero = absent (v1 records, unbound trials).
    std::uint64_t baseline_gen;     // host workspace generation at trial open
    std::uint64_t candidate_hash;   // FNV-1a of the trial's patch bytes
    std::uint64_t workload_sym;     // benchmark workload / phase identity
    std::uint64_t seed;             // replay random seed
    std::uint64_t verifier_version; // judge / verifier version
};

struct ConstraintSetHeader {
    std::uint64_t version;
    std::uint64_t object_off;
    std::uint64_t object_len;
    std::uint64_t schema;
};

struct AuditRecord {
    std::uint64_t seq;
    std::uint16_t kind;
    std::uint16_t reserved;
    std::uint32_t actor;
    std::uint64_t target;
    std::uint64_t value;
    std::uint64_t generation;
};

static_assert(sizeof(ImageHeader) == kImageHeaderSize);
static_assert(offsetof(ImageHeader, committed_gen) == 64);
static_assert(offsetof(ImageHeader, bell) == 128);
static_assert(offsetof(ImageHeader, section_dir_off) == 192);
static_assert(sizeof(SectionEntry) == 64);
static_assert(sizeof(StringTableEntry) == 16);
static_assert(sizeof(SymbolEntry) == 16);
static_assert(sizeof(NodeRecord) == 64);
static_assert(sizeof(PatchRecord) == 40);
static_assert(sizeof(MetricRecord) == 24);
static_assert(sizeof(CctRecord) == 48);
static_assert(sizeof(WorldlineRecord) == 88);
static_assert(offsetof(WorldlineRecord, baseline_gen) == 48);
static_assert(sizeof(ConstraintSetHeader) == 32);
static_assert(sizeof(AuditRecord) == 40);

// v1 frozen field counts. A schema id hashes only this prefix, so a
// later layout that appends fields keeps the id and bumps
// layout_version; readers default the unknown tail.
inline constexpr std::size_t kNodeFrozenFields = 11;
inline constexpr std::size_t kPatchFrozenFields = 5;
inline constexpr std::size_t kMetricFrozenFields = 5;
inline constexpr std::size_t kCctFrozenFields = 6;
inline constexpr std::size_t kWorldlineFrozenFields = 9;
inline constexpr std::size_t kAuditFrozenFields = 6;
inline constexpr std::size_t kStringEntryFrozenFields = 3;
inline constexpr std::size_t kSymbolEntryFrozenFields = 3;
inline constexpr std::size_t kConstraintFrozenFields = 4;

// #5379: layout version 2 appends the WorldlineRecord evidence tail.
// v1 images stay readable: element_size 48 records default the tail.
inline constexpr std::uint64_t kImageLayoutVersion = 2;

// The v1 worldline prefix width. A v1 image's worldline section uses
// element_size 48; readers copy the prefix and default the evidence
// tail to zero (#5379 additive-evolution rule).
inline constexpr std::uint32_t kWorldlineV1Size = 48;

// #5378: the host string table reserves this many entry slots; the byte
// region follows the fixed array. string_table_append uses it to keep
// the entry array size constant so validation's count*16 bound holds.
inline constexpr std::uint64_t kStringSlotsReserved = 1024;

inline constexpr std::uint32_t expected_element_size(SectionKind kind) {
    switch (kind) {
        case SectionKind::StringTable:
            return sizeof(StringTableEntry);
        case SectionKind::SymbolTable:
            return sizeof(SymbolEntry);
        case SectionKind::Nodes:
            return sizeof(NodeRecord);
        case SectionKind::Patches:
            return sizeof(PatchRecord);
        case SectionKind::Metrics:
            return sizeof(MetricRecord);
        case SectionKind::Cct:
            return sizeof(CctRecord);
        case SectionKind::Worldlines:
            return sizeof(WorldlineRecord);
        case SectionKind::Constraints:
            return sizeof(ConstraintSetHeader);
        case SectionKind::Audit:
            return sizeof(AuditRecord);
        case SectionKind::Staging:
            return 1;
    }
    return 0;
}

inline constexpr bool is_record_section(SectionKind kind) {
    return kind != SectionKind::StringTable && kind != SectionKind::Staging &&
           kind != SectionKind::Constraints;
}

} // namespace aura::bus

#endif
