// ──────────────────────────────────────────────────────────────
//  image.cpp — bus image validation (#5122)
//
//  The mapping is untrusted: magic, endianness, header length, sealed
//  size, schema id, section bounds/alignment, element sizes, the
//  bit-63 immediate rule and every cross-section index are checked
//  here. No allocation, no serialization.
// ──────────────────────────────────────────────────────────────
#include "bus/image.hh"

#include "bus/image_schema.hh"

#include <algorithm>
#include <array>
#include <atomic>

namespace aura::bus {
namespace {

    constexpr bool in_bounds(std::uint64_t off, std::uint64_t len, std::uint64_t total) {
        return off <= total && len <= total - off;
    }

    constexpr bool even(std::uint64_t v) {
        return (v & 1u) == 0;
    }

    std::uint64_t fnv_section(std::span<const std::byte> image, std::uint64_t off,
                              std::uint64_t len) {
        std::uint64_t hash = 14695981039346656037ull;
        for (std::uint64_t i = 0; i < len; ++i) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(image[off + i]));
            hash *= 1099511628211ull;
        }
        return hash;
    }

    template <typename T> T load_at(std::span<const std::byte> image, std::uint64_t off) {
        T v{};
        if (in_bounds(off, sizeof(T), image.size()))
            std::memcpy(&v, image.data() + off, sizeof(T));
        return v;
    }

    struct SectionView {
        bool present = false;
        SectionEntry entry{};
        std::uint64_t count = 0;
    };

    SectionView view_section(std::span<const std::byte> image, const ImageHeader& h,
                             SectionKind kind) {
        SectionView v{};
        for (std::uint32_t i = 0; i < h.section_count; ++i) {
            const SectionEntry e = load_section_entry(image, i);
            if (e.kind == static_cast<std::uint32_t>(kind)) {
                v.present = true;
                v.entry = e;
                v.count = e.count;
                return v;
            }
        }
        return v;
    }

    const std::byte* section_base(std::span<const std::byte> image, const SectionEntry& e) {
        return image.data() + e.offset;
    }

    // Parent indexes must point strictly backwards, so a cycle is
    // structurally impossible and the walk is bounded by the record count.
    template <typename ParentFn>
    bool backwards_parents(std::uint64_t count, ParentFn parent_of, std::uint64_t sentinel) {
        for (std::uint64_t i = 0; i < count; ++i) {
            const std::uint64_t p = parent_of(i);
            if (p == sentinel)
                continue;
            if (p >= i)
                return false;
        }
        return true;
    }

} // namespace

ImageCheck validate_image(std::span<const std::byte> image) {
    if (image.size() < kImageHeaderSize)
        return {ImageStatus::Truncated, "image is smaller than the 256-byte header"};
    const ImageHeader h = load_header(image);

    if (std::memcmp(h.magic, kImageMagic, sizeof(h.magic)) != 0)
        return {ImageStatus::BadMagic, "magic is not AURAIMG"};
    if (h.endianness != kImageEndianMarker)
        return {ImageStatus::BadEndian, "byte-order marker is not little-endian"};
    if (h.header_len != kImageHeaderSize)
        return {ImageStatus::BadHeader, "header length is not 256"};
    if (h.abi_major != kImageAbiMajor)
        return {ImageStatus::BadAbi, "unsupported ABI major"};
    if (h.image_size != image.size())
        return {ImageStatus::BadSize, "sealed image size does not match the mapping"};
    const SchemaId expected_header = header_schema_id();
    std::array<std::uint8_t, 16> expected_bytes{};
    std::memcpy(expected_bytes.data(), &expected_header.a, 8);
    std::memcpy(expected_bytes.data() + 8, &expected_header.b, 8);
    if (std::memcmp(h.schema_id, expected_bytes.data(), expected_bytes.size()) != 0)
        return {ImageStatus::BadSchema, "schema id does not match this reader"};
    if (!even(h.committed_gen))
        return {ImageStatus::BadGeneration, "committed generation is not stable (odd)"};
    if (h.committed_bytes < kImageHeaderSize || h.committed_bytes > image.size() ||
        h.committed_bytes % 8 != 0)
        return {ImageStatus::BadGeneration, "committed bytes outside the sealed image"};
    if (h.section_count > 64)
        return {ImageStatus::BadDirectory, "too many section entries"};
    if (h.section_dir_off < kImageHeaderSize || h.section_dir_off % 8 != 0 ||
        !in_bounds(h.section_dir_off,
                   static_cast<std::uint64_t>(h.section_count) * sizeof(SectionEntry),
                   h.committed_bytes))
        return {ImageStatus::BadDirectory, "section directory outside the committed prefix"};

    // ── Directory pass ───────────────────────────────────────────
    // #5399: Tasks is the newest known kind. The header schema id does
    // not include it, so a pre-v0.3.3 reader sees an unknown section
    // and skips it (below) while a current reader validates it.
    bool seen[static_cast<std::size_t>(SectionKind::Tasks) + 1] = {};
    std::uint64_t total_records = 0;
    for (std::uint32_t i = 0; i < h.section_count; ++i) {
        const SectionEntry e = load_section_entry(image, i);
        if (e.offset < kImageHeaderSize || e.offset % 8 != 0)
            return {ImageStatus::BadAlignment, "section offset is not 8-aligned past the header"};
        if (!in_bounds(e.offset, e.capacity, h.committed_bytes))
            return {ImageStatus::BadBounds, "section capacity lies outside the committed prefix"};
        if (e.length > e.capacity)
            return {ImageStatus::BadBounds, "section length exceeds its capacity"};
        if (e.element_size == 0)
            return {ImageStatus::BadElementSize, "section element size is zero"};
        if (e.kind > static_cast<std::uint32_t>(SectionKind::Tasks)) {
            // Unknown section kind: ignored by this version, but still
            // bounds-checked above.
            continue;
        }
        const auto kind = static_cast<SectionKind>(e.kind);
        const auto slot = static_cast<std::size_t>(kind);
        if (seen[slot])
            return {ImageStatus::BadDuplicateSection, "duplicate section kind"};
        seen[slot] = true;
        if (e.version > 1)
            return {ImageStatus::BadSection, "unsupported section version"};
        const std::uint64_t expected_size = expected_element_size(kind);
        // #5379: the worldline record grew an evidence tail in layout v2.
        // A v1 image (48-byte worldlines) stays valid: the frozen prefix
        // is the v1 width, and readers default the unknown tail to zero.
        const std::uint64_t min_size =
            kind == SectionKind::Worldlines ? kWorldlineV1Size : expected_size;
        if (e.element_size < min_size)
            return {ImageStatus::BadElementSize, "section element size is narrower than v1"};
        if (is_record_section(kind)) {
            if (e.capacity % e.element_size != 0)
                return {ImageStatus::BadElementSize, "record section capacity is not a multiple"};
            if (e.length % e.element_size != 0 || e.count != e.length / e.element_size)
                return {ImageStatus::BadElementSize,
                        "record section count does not match its length"};
            if (e.count > kImageMaxRecords)
                return {ImageStatus::BadVisitLimit, "record section exceeds the visit cap"};
            total_records += e.count;
        }
        if (kind == SectionKind::StringTable) {
            if (static_cast<std::uint64_t>(e.count) * sizeof(StringTableEntry) > e.length)
                return {ImageStatus::BadElementSize, "string-table entries exceed the section"};
            if (e.count > kImageMaxRecords)
                return {ImageStatus::BadVisitLimit, "string table exceeds the visit cap"};
        }
        if (kind == SectionKind::Constraints && e.length < sizeof(ConstraintSetHeader))
            return {ImageStatus::BadSection, "constraints section is shorter than its header"};
        if (kind != SectionKind::Staging && e.fingerprint != section_layout_fingerprint(kind))
            return {ImageStatus::BadFingerprint, "section layout fingerprint mismatch"};
    }
    if (total_records > kImageMaxTotalRecords)
        return {ImageStatus::BadVisitLimit, "image exceeds the total record cap"};

    const SectionView strings = view_section(image, h, SectionKind::StringTable);
    const SectionView symbols = view_section(image, h, SectionKind::SymbolTable);
    const SectionView nodes = view_section(image, h, SectionKind::Nodes);
    const SectionView patches = view_section(image, h, SectionKind::Patches);
    const SectionView metrics = view_section(image, h, SectionKind::Metrics);
    const SectionView cct = view_section(image, h, SectionKind::Cct);
    const SectionView worldlines = view_section(image, h, SectionKind::Worldlines);

    // ── String table: entries index UTF-8 bytes inside the section ──
    // The entry array is a FIXED reserved region (kStringSlotsReserved
    // slots); bytes follow it. Validate each entry's byte range against
    // the section length, and require the offset past the reserved array
    // (not count*16, which grows as entries are added).
    if (strings.present) {
        const std::uint64_t reserved =
            (kStringSlotsReserved * sizeof(StringTableEntry) + 7) & ~7ull;
        for (std::uint64_t i = 0; i < strings.count; ++i) {
            const StringTableEntry s = load_at<StringTableEntry>(
                image, strings.entry.offset + i * strings.entry.element_size);
            if (s.off % 8 != 0 || s.off < reserved ||
                !in_bounds(s.off, s.len, strings.entry.length))
                return {ImageStatus::BadBounds, "string-table entry outside the section"};
        }
    }

    // ── Symbol table: names point into the string table ──────────
    if (symbols.present) {
        if (!strings.present)
            return {ImageStatus::BadIndex, "symbol table without a string table"};
        for (std::uint64_t i = 0; i < symbols.count; ++i) {
            const SymbolEntry s =
                load_at<SymbolEntry>(image, symbols.entry.offset + i * symbols.entry.element_size);
            if (!in_bounds(s.name_off, s.name_len, strings.entry.length))
                return {ImageStatus::BadIndex, "symbol name outside the string table"};
        }
    }

    // ── Nodes: index edges only ─────────────────────────────────
    if (nodes.present) {
        for (std::uint64_t i = 0; i < nodes.count; ++i) {
            const NodeRecord n =
                load_at<NodeRecord>(image, nodes.entry.offset + i * nodes.entry.element_size);
            const auto bad_edge = [&](std::uint64_t v) {
                return v != kImageNoIndex && v >= nodes.count;
            };
            if (bad_edge(n.parent) || bad_edge(n.first_child) || bad_edge(n.next_sibling))
                return {ImageStatus::BadIndex, "node edge outside the node section"};
            if (n.sym != kImageNoIndex && (!symbols.present || n.sym >= symbols.count))
                return {ImageStatus::BadIndex, "node symbol outside the symbol table"};
        }
    }

    // ── Patches: immediate bit 63 clear, indexes in range ───────
    if (patches.present) {
        for (std::uint64_t i = 0; i < patches.count; ++i) {
            const PatchRecord p =
                load_at<PatchRecord>(image, patches.entry.offset + i * patches.entry.element_size);
            if ((p.new_value >> 63) != 0)
                return {ImageStatus::BadImmediate, "patch value carries bit 63"};
            if (p.node != kImageNoIndex && (!nodes.present || p.node >= nodes.count))
                return {ImageStatus::BadIndex, "patch node outside the node section"};
            // A patch can be committed before the worldline record that
            // refers to it; the id only has to fit the reserved table.
            const std::uint64_t wl_slots =
                worldlines.present && worldlines.entry.element_size
                    ? worldlines.entry.capacity / worldlines.entry.element_size
                    : 0;
            if (p.worldline != kImageNoIndex && p.worldline >= wl_slots)
                return {ImageStatus::BadIndex, "patch worldline outside the worldline table"};
        }
    }

    // ── Metrics: immediate bit 63 clear, key in the symbol table ─
    if (metrics.present) {
        for (std::uint64_t i = 0; i < metrics.count; ++i) {
            const MetricRecord m =
                load_at<MetricRecord>(image, metrics.entry.offset + i * metrics.entry.element_size);
            if ((m.value >> 63) != 0)
                return {ImageStatus::BadImmediate, "metric value carries bit 63"};
            // key_sym carries the publisher's integer key (the std/bus
            // contract: keys are immediates, not symbol-table indexes);
            // kImageNoIndex means "no key".
            if (m.key_sym != kImageNoIndex && (m.key_sym >> 63) != 0)
                return {ImageStatus::BadImmediate, "metric key carries bit 63"};
        }
    }

    // ── CCT: mid low 62 bits, backward parents (acyclic, bounded) ─
    if (cct.present) {
        for (std::uint64_t i = 0; i < cct.count; ++i) {
            const CctRecord c =
                load_at<CctRecord>(image, cct.entry.offset + i * cct.entry.element_size);
            if ((c.mid_lo >> 62) != 0)
                return {ImageStatus::BadImmediate, "CCT mid carries bits above the low 62"};
            if (c.node != kImageNoIndex && (!nodes.present || c.node >= nodes.count))
                return {ImageStatus::BadIndex, "CCT node outside the node section"};
        }
        const auto parent = [&](std::uint64_t i) {
            return load_at<CctRecord>(image, cct.entry.offset + i * cct.entry.element_size).parent;
        };
        if (!backwards_parents(cct.count, parent, kImageNoParent))
            return {ImageStatus::BadCycle, "CCT parent does not point backwards"};
    }

    // ── Worldlines: backward parents, state domain, patch range ──
    if (worldlines.present) {
        for (std::uint64_t i = 0; i < worldlines.count; ++i) {
            WorldlineRecord w{};
            std::memcpy(&w,
                        image.data() + worldlines.entry.offset + i * worldlines.entry.element_size,
                        std::min<std::uint64_t>(worldlines.entry.element_size, sizeof(w)));
            if (w.state > static_cast<std::uint8_t>(WorldlineState::Canary))
                return {ImageStatus::BadSection, "worldline state outside the domain"};
            if (w.constraint_result > static_cast<std::uint8_t>(ConstraintResult::Fail))
                return {ImageStatus::BadSection, "worldline constraint result outside the domain"};
            if (w.patch_len != 0) {
                // Patches and worldlines commit independently; the range
                // must fit the reserved patch capacity, not only the
                // currently committed length.
                if (!patches.present ||
                    !in_bounds(w.patch_off, w.patch_len, patches.entry.capacity))
                    return {ImageStatus::BadBounds,
                            "worldline patch range outside the patch section"};
            }
            if (w.metrics_off != kImageNoIndex &&
                (!metrics.present || w.metrics_off > metrics.entry.length))
                return {ImageStatus::BadIndex,
                        "worldline metric offset outside the metric section"};
        }
        const auto parent = [&](std::uint64_t i) {
            WorldlineRecord w{};
            std::memcpy(&w,
                        image.data() + worldlines.entry.offset + i * worldlines.entry.element_size,
                        std::min<std::uint64_t>(worldlines.entry.element_size, sizeof(w)));
            return w.parent;
        };
        if (!backwards_parents(worldlines.count, parent, kImageNoParent))
            return {ImageStatus::BadCycle, "worldline parent does not point backwards"};
    }

    // ── Audit: append-only sequence, committed generations only ──
    const SectionView audit = view_section(image, h, SectionKind::Audit);
    if (audit.present) {
        std::uint64_t prev = 0;
        for (std::uint64_t i = 0; i < audit.count; ++i) {
            const AuditRecord a =
                load_at<AuditRecord>(image, audit.entry.offset + i * audit.entry.element_size);
            if (i > 0 && a.seq <= prev)
                return {ImageStatus::BadSection, "audit sequence is not increasing"};
            if (!even(a.generation))
                return {ImageStatus::BadGeneration, "audit entry records an unstable generation"};
            prev = a.seq;
        }
    }

    // ── Constraints: header plus an inline executable object ─────
    const SectionView constraints = view_section(image, h, SectionKind::Constraints);
    if (constraints.present) {
        const ConstraintSetHeader cs =
            load_at<ConstraintSetHeader>(image, constraints.entry.offset);
        if (!in_bounds(cs.object_off, cs.object_len, constraints.entry.length) ||
            cs.object_off < sizeof(ConstraintSetHeader))
            return {ImageStatus::BadBounds, "constraint object outside the section"};
    }

    // Committed section bytes are immutable in that generation.
    for (std::uint32_t i = 0; i < h.section_count; ++i) {
        const SectionEntry e = load_section_entry(image, i);
        if (e.kind > static_cast<std::uint32_t>(SectionKind::Tasks))
            continue;
        if (static_cast<SectionKind>(e.kind) == SectionKind::Staging)
            continue;
        if (fnv_section(image, e.offset, e.length) != e.digest)
            return {ImageStatus::BadDigest, "committed section bytes changed in place"};
    }

    return {ImageStatus::Ok, ""};
}

std::uint64_t committed_digest(std::span<const std::byte> image) {
    const ImageHeader h = load_header(image);
    const std::uint64_t end = h.committed_bytes <= image.size() ? h.committed_bytes : image.size();
    std::uint64_t hash = 14695981039346656037ull;
    for (std::uint64_t i = 0; i < end; ++i) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(image[i]));
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t audit_count(std::span<const std::byte> image) {
    SectionEntry e{};
    if (!find_section(image, SectionKind::Audit, &e))
        return 0;
    return e.count;
}

// #5378: source-carrier string table. Layout: a fixed entry array at the
// section head (kStringSlots slots reserved by the builder), then inline
// UTF-8 bytes appended after the last committed byte. Entries never
// move; a new entry fills the next free slot and its bytes land at the
// committed tail. The builder sets length = the reserved array size, so
// the byte region starts at length and grows with each append.
// Exhaustion (no slot or no byte room) is a hard error.
bool string_table_append(std::span<std::byte> image, std::string_view text, std::uint64_t* index,
                         std::string* why) {
    const auto fail = [&](const char* msg) {
        if (why)
            *why = msg;
        return false;
    };
    SectionEntry e{};
    if (!find_section(image, SectionKind::StringTable, &e))
        return fail("string table section is missing");
    if (e.element_size != sizeof(StringTableEntry))
        return fail("string table element size mismatch");
    // Layout: a fixed reserved entry array (kStringSlotsReserved slots),
    // then the byte region. Entries fill the array from slot 0; bytes
    // append at the committed tail, 8-aligned. The array size is FIXED so
    // existing entries never move.
    const std::uint64_t reserved = (kStringSlotsReserved * sizeof(StringTableEntry) + 7) & ~7ull;
    const std::uint64_t tail = e.length < reserved ? reserved : e.length;
    const std::uint64_t byte_off = (tail + 7) & ~7ull;
    if ((e.count + 1) * sizeof(StringTableEntry) > reserved)
        return fail("string table entry slots exhausted");
    if (text.size() > kImageMaxRecords)
        return fail("source text is too large");
    const std::uint64_t aligned_len = (text.size() + 7) & ~7ull;
    if (byte_off > e.capacity || aligned_len > e.capacity - byte_off)
        return fail("string table is full");
    StringTableEntry ent{};
    ent.off = byte_off;
    ent.len = static_cast<std::uint32_t>(text.size());
    std::memcpy(image.data() + e.offset + e.count * sizeof(StringTableEntry), &ent, sizeof(ent));
    if (!text.empty())
        std::memcpy(image.data() + e.offset + byte_off, text.data(), text.size());
    ImageHeader h = load_header(image);
    const std::uint64_t next_gen = h.committed_gen + 2;
    e.length = byte_off + aligned_len;
    e.count += 1;
    e.generation = next_gen;
    e.digest = fnv_section(image, e.offset, e.length);
    std::uint64_t at = 0;
    for (std::uint32_t i = 0; i < h.section_count; ++i) {
        const SectionEntry cand = load_section_entry(image, i);
        if (cand.kind == static_cast<std::uint32_t>(SectionKind::StringTable)) {
            at = h.section_dir_off + static_cast<std::uint64_t>(i) * sizeof(SectionEntry);
            break;
        }
    }
    if (at == 0)
        return fail("string table directory entry vanished");
    std::memcpy(image.data() + at, &e, sizeof(e));
    std::atomic_thread_fence(std::memory_order_release);
    h.committed_gen = next_gen;
    std::memcpy(image.data(), &h, sizeof(h));
    *index = e.count - 1;
    return true;
}

bool string_table_read(std::span<const std::byte> image, std::uint64_t index,
                       std::string_view* text) {
    SectionEntry e{};
    if (!find_section(image, SectionKind::StringTable, &e))
        return false;
    if (index >= e.count || e.element_size != sizeof(StringTableEntry))
        return false;
    const StringTableEntry ent =
        load_at<StringTableEntry>(image, e.offset + index * e.element_size);
    if (!in_bounds(ent.off, ent.len, e.length))
        return false;
    *text =
        std::string_view(reinterpret_cast<const char*>(image.data() + e.offset + ent.off), ent.len);
    return true;
}

bool audit_append(std::span<std::byte> image, AuditRecord* record, std::string* why) {
    const auto fail = [&](const char* text) {
        if (why)
            *why = text;
        return false;
    };
    if (record == nullptr)
        return fail("null audit record");
    SectionEntry e{};
    if (!find_section(image, SectionKind::Audit, &e))
        return fail("audit section is missing");
    if (e.element_size != sizeof(AuditRecord))
        return fail("audit element size mismatch");
    if (e.length > e.capacity || sizeof(AuditRecord) > e.capacity - e.length)
        return fail("audit section is full");
    if (e.count > 0) {
        const AuditRecord last =
            load_at<AuditRecord>(image, e.offset + (e.count - 1) * e.element_size);
        if (record->seq <= last.seq)
            return fail("audit sequence does not increase");
    }
    ImageHeader h = load_header(image);
    const std::uint64_t next_gen = h.committed_gen + 2;
    record->generation = next_gen;
    std::memcpy(image.data() + e.offset + e.length, record, sizeof(*record));
    e.length += sizeof(*record);
    e.count += 1;
    e.generation = next_gen;
    e.digest = fnv_section(image, e.offset, e.length);
    std::uint64_t at = 0;
    const ImageHeader dh = load_header(image);
    for (std::uint32_t i = 0; i < dh.section_count; ++i) {
        const SectionEntry cand = load_section_entry(image, i);
        if (cand.kind == static_cast<std::uint32_t>(SectionKind::Audit)) {
            at = dh.section_dir_off + static_cast<std::uint64_t>(i) * sizeof(SectionEntry);
            break;
        }
    }
    if (at == 0)
        return fail("audit directory entry vanished");
    std::memcpy(image.data() + at, &e, sizeof(e));
    std::atomic_thread_fence(std::memory_order_release);
    h.committed_gen = next_gen;
    std::memcpy(image.data(), &h, sizeof(h));
    return true;
}

bool constraint_write(std::span<std::byte> image, std::uint64_t version,
                      std::span<const std::byte> blob, std::string* why) {
    const auto fail = [&](const char* text) {
        if (why)
            *why = text;
        return false;
    };
    SectionEntry e{};
    if (!find_section(image, SectionKind::Constraints, &e))
        return fail("constraints section is missing");
    if (e.length > e.capacity || blob.size() > e.capacity - sizeof(ConstraintSetHeader))
        return fail("constraint object exceeds the reserved section");
    std::uint64_t at = 0;
    const ImageHeader dh = load_header(image);
    for (std::uint32_t i = 0; i < dh.section_count; ++i) {
        const SectionEntry cand = load_section_entry(image, i);
        if (cand.kind == static_cast<std::uint32_t>(SectionKind::Constraints)) {
            at = dh.section_dir_off + static_cast<std::uint64_t>(i) * sizeof(SectionEntry);
            break;
        }
    }
    if (at == 0)
        return fail("constraints directory entry vanished");
    ConstraintSetHeader ch{};
    ch.version = version;
    ch.object_off = sizeof(ConstraintSetHeader);
    ch.object_len = blob.size();
    ch.schema = 1;
    std::memcpy(image.data() + e.offset, &ch, sizeof(ch));
    if (!blob.empty())
        std::memcpy(image.data() + e.offset + sizeof(ch), blob.data(), blob.size());
    ImageHeader h = load_header(image);
    const std::uint64_t next_gen = h.committed_gen + 2;
    e.length = sizeof(ch) + blob.size();
    e.count = 0;
    e.generation = next_gen;
    e.digest = fnv_section(image, e.offset, e.length);
    std::memcpy(image.data() + at, &e, sizeof(e));
    std::atomic_thread_fence(std::memory_order_release);
    h.committed_gen = next_gen;
    std::memcpy(image.data(), &h, sizeof(h));
    return true;
}

bool constraint_read(std::span<const std::byte> image, std::uint64_t* version,
                     std::span<const std::byte>* blob) {
    SectionEntry e{};
    if (!find_section(image, SectionKind::Constraints, &e) ||
        e.length < sizeof(ConstraintSetHeader))
        return false;
    ConstraintSetHeader ch{};
    std::memcpy(&ch, image.data() + e.offset, sizeof(ch));
    if (ch.object_off < sizeof(ConstraintSetHeader) || ch.object_off > e.length ||
        ch.object_len > e.length - ch.object_off)
        return false;
    if (version)
        *version = ch.version;
    if (blob)
        *blob = {image.data() + e.offset + ch.object_off, static_cast<std::size_t>(ch.object_len)};
    return true;
}

} // namespace aura::bus
