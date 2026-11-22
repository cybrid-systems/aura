// ──────────────────────────────────────────────────────────────
//  image.hh — relocatable bus image reader/validator (#5122)
//
//  Header + section directory + fixed-width record traversal. Every
//  read is bounds-checked; an image is untrusted input even from the
//  same uid. No allocation and no auto_serialize on the read path.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_IMAGE_HH
#define AURA_BUS_IMAGE_HH

#include "bus/image_format.hh"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace aura::bus {

enum class ImageStatus {
    Ok = 0,
    Truncated,
    BadMagic,
    BadEndian,
    BadHeader,
    BadSize,
    BadAbi,
    BadSchema,
    BadGeneration,
    BadDirectory,
    BadSection,
    BadBounds,
    BadAlignment,
    BadElementSize,
    BadFingerprint,
    BadImmediate,
    BadIndex,
    BadCycle,
    BadVisitLimit,
    BadDuplicateSection,
    BadDigest,
};

struct ImageCheck {
    ImageStatus status = ImageStatus::Ok;
    const char* detail = "";

    [[nodiscard]] bool ok() const noexcept { return status == ImageStatus::Ok; }
};

inline constexpr std::string_view image_status_name(ImageStatus s) {
    switch (s) {
        case ImageStatus::Ok:
            return "ok";
        case ImageStatus::Truncated:
            return "truncated";
        case ImageStatus::BadMagic:
            return "bad-magic";
        case ImageStatus::BadEndian:
            return "bad-endian";
        case ImageStatus::BadHeader:
            return "bad-header";
        case ImageStatus::BadSize:
            return "bad-size";
        case ImageStatus::BadAbi:
            return "bad-abi";
        case ImageStatus::BadSchema:
            return "bad-schema";
        case ImageStatus::BadGeneration:
            return "bad-generation";
        case ImageStatus::BadDirectory:
            return "bad-directory";
        case ImageStatus::BadSection:
            return "bad-section";
        case ImageStatus::BadBounds:
            return "bad-bounds";
        case ImageStatus::BadAlignment:
            return "bad-alignment";
        case ImageStatus::BadElementSize:
            return "bad-element-size";
        case ImageStatus::BadFingerprint:
            return "bad-fingerprint";
        case ImageStatus::BadImmediate:
            return "bad-immediate";
        case ImageStatus::BadIndex:
            return "bad-index";
        case ImageStatus::BadCycle:
            return "bad-cycle";
        case ImageStatus::BadVisitLimit:
            return "bad-visit-limit";
        case ImageStatus::BadDuplicateSection:
            return "bad-duplicate-section";
        case ImageStatus::BadDigest:
            return "bad-digest";
    }
    return "unknown";
}

inline ImageHeader load_header(std::span<const std::byte> image) {
    ImageHeader h{};
    if (image.size() >= sizeof(ImageHeader))
        std::memcpy(&h, image.data(), sizeof(ImageHeader));
    return h;
}

inline SectionEntry load_section_entry(std::span<const std::byte> image, std::uint64_t index) {
    SectionEntry e{};
    const ImageHeader h = load_header(image);
    const std::uint64_t at = h.section_dir_off + index * sizeof(SectionEntry);
    if (at + sizeof(SectionEntry) <= image.size())
        std::memcpy(&e, image.data() + at, sizeof(SectionEntry));
    return e;
}

// Locate one known section kind. Returns false when absent.
inline bool find_section(std::span<const std::byte> image, SectionKind kind, SectionEntry* out) {
    const ImageHeader h = load_header(image);
    if (h.section_count > 4096)
        return false;
    for (std::uint32_t i = 0; i < h.section_count; ++i) {
        const SectionEntry e = load_section_entry(image, i);
        if (e.kind == static_cast<std::uint32_t>(kind)) {
            *out = e;
            return true;
        }
    }
    return false;
}

// Walk a record section without allocating. The callback receives the
// record pointer, the section's element size and the index. The caller
// must have validated the image first.
template <typename Fn>
ImageStatus visit_section(std::span<const std::byte> image, SectionKind kind, Fn&& fn) {
    SectionEntry e{};
    if (!find_section(image, kind, &e))
        return ImageStatus::BadSection;
    if (e.element_size == 0 || e.offset > image.size() || e.length > image.size() - e.offset)
        return ImageStatus::BadBounds;
    if (e.length % e.element_size != 0)
        return ImageStatus::BadElementSize;
    const std::uint64_t count = e.count;
    if (count > kImageMaxRecords)
        return ImageStatus::BadVisitLimit;
    const std::byte* base = image.data() + e.offset;
    for (std::uint64_t i = 0; i < count; ++i)
        fn(base + i * e.element_size, e.element_size, i);
    return ImageStatus::Ok;
}

// Read one record prefix without copying the whole image.
template <typename T>
bool read_record(const std::byte* record, std::uint64_t element_size, T* out) {
    if (element_size < sizeof(T))
        return false;
    std::memcpy(out, record, sizeof(T));
    return true;
}

// Validate every header field, directory entry, known section and
// cross-section index. Safe on an untrusted mapping of any size.
ImageCheck validate_image(std::span<const std::byte> image);

// FNV-1a 64 over the committed prefix, for golden comparisons.
std::uint64_t committed_digest(std::span<const std::byte> image);

// Coordinator-side audit append (#5122/#5126). The audit section is
// append-only: the record lands in the reserved tail, the section and the
// header advance to the next even committed generation, and the section
// digest is recomputed. Returns false when there is no audit section, no
// room, or the sequence does not increase.
bool audit_append(std::span<std::byte> image, AuditRecord* record, std::string* why);

// Committed audit-record count (0 when the builder reserved no section).
std::uint64_t audit_count(std::span<const std::byte> image);

// #5378: source-carrier string table. The coordinator appends a UTF-8
// source text into the reserved tail of the StringTable section and
// returns its entry index; a PatchRecord with field_offset
// kPatchSourceField carries that index as new_value. Append-only, the
// section and header advance to the next even committed generation.
// Returns false when there is no string section or no room.
bool string_table_append(std::span<std::byte> image, std::string_view text, std::uint64_t* index,
                         std::string* why);
// Read one string-table entry back (bounds-checked). False when absent.
bool string_table_read(std::span<const std::byte> image, std::uint64_t index,
                       std::string_view* text);
// Patch field_offset marker: new_value is a StringTable entry index
// holding the candidate define source, not an immediate field value.
inline constexpr std::uint32_t kPatchSourceField = 0xFFFFFFF0u;

// Versioned constraint object (#5126): the coordinator writes the current
// set (header + opaque EDSL object blob) into the constraints section and
// advances the committed generation. Readers get the version and the
// blob; decoding the blob is the caller's format.
bool constraint_write(std::span<std::byte> image, std::uint64_t version,
                      std::span<const std::byte> blob, std::string* why);
bool constraint_read(std::span<const std::byte> image, std::uint64_t* version,
                     std::span<const std::byte>* blob);

} // namespace aura::bus

#endif
