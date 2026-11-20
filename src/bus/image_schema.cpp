// ──────────────────────────────────────────────────────────────
//  image_schema.cpp — compile-time schema ids (#5122)
//
//  P2996 lives in this non-import-std TU (the aura-reflect placement
//  rule). No module imports, no runtime reflection.
// ──────────────────────────────────────────────────────────────
#include "bus/image_schema.hh"

#include "reflect/reflect.hh"

#include <array>
#include <meta>
#include <string_view>

namespace aura::bus {
namespace {

    consteval void hash_bytes(std::uint64_t& h, std::string_view s) {
        for (unsigned char c : s) {
            h ^= static_cast<std::uint64_t>(c);
            h *= 1099511628211ull;
        }
    }

    consteval void hash_u64(std::uint64_t& h, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xffu;
            h *= 1099511628211ull;
        }
    }

    consteval void hash_u8(std::uint64_t& h, std::uint8_t v) {
        h ^= v;
        h *= 1099511628211ull;
    }

    // Digest the frozen field prefix: name, offset, width, signedness,
    // enum-ness, in declaration order. Two independent streams make a
    // 128-bit id; a collision needs both streams to collide.
    consteval SchemaId digest_type(std::meta::info type, std::size_t frozen) {
        std::uint64_t a = 14695981039346656037ull;
        std::uint64_t b = 0x9e3779b97f4a7c15ull;
        auto members =
            std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked());
        if (frozen > members.size())
            return {};
        for (std::size_t i = 0; i < frozen; ++i) {
            const auto member = members[i];
            const std::string_view name = std::meta::identifier_of(member);
            const std::uint64_t off =
                static_cast<std::uint64_t>(std::meta::offset_of(member).bytes);
            const std::uint64_t width =
                static_cast<std::uint64_t>(std::meta::size_of(std::meta::type_of(member)));
            const std::uint8_t is_signed =
                std::meta::is_signed_type(std::meta::type_of(member)) ? 1u : 0u;
            const std::uint8_t is_enum =
                std::meta::is_enum_type(std::meta::type_of(member)) ? 1u : 0u;
            hash_bytes(a, name);
            hash_u64(a, off);
            hash_u64(a, width);
            hash_u8(a, is_signed);
            hash_u8(a, is_enum);
            // Second stream mixes in a different order (width first) so a
            // reordered layout cannot produce the same pair.
            hash_u64(b, width);
            hash_u64(b, off);
            hash_bytes(b, name);
            hash_u8(b, is_enum);
            hash_u8(b, is_signed);
        }
        return {a, b};
    }

    consteval SchemaId section_schema(SectionKind kind) {
        switch (kind) {
            case SectionKind::StringTable:
                return digest_type(^^StringTableEntry, kStringEntryFrozenFields);
            case SectionKind::SymbolTable:
                return digest_type(^^SymbolEntry, kSymbolEntryFrozenFields);
            case SectionKind::Nodes:
                return digest_type(^^NodeRecord, kNodeFrozenFields);
            case SectionKind::Patches:
                return digest_type(^^PatchRecord, kPatchFrozenFields);
            case SectionKind::Metrics:
                return digest_type(^^MetricRecord, kMetricFrozenFields);
            case SectionKind::Cct:
                return digest_type(^^CctRecord, kCctFrozenFields);
            case SectionKind::Worldlines:
                return digest_type(^^WorldlineRecord, kWorldlineFrozenFields);
            case SectionKind::Constraints:
                return digest_type(^^ConstraintSetHeader, kConstraintFrozenFields);
            case SectionKind::Audit:
                return digest_type(^^AuditRecord, kAuditFrozenFields);
            case SectionKind::Staging:
                return {}; // raw bytes: no schema
        }
        return {};
    }

    inline constexpr SectionKind kKnownKinds[] = {
        SectionKind::StringTable, SectionKind::SymbolTable, SectionKind::Nodes,
        SectionKind::Patches,     SectionKind::Metrics,     SectionKind::Cct,
        SectionKind::Worldlines,  SectionKind::Constraints, SectionKind::Audit,
        SectionKind::Staging,
    };

    consteval SchemaId header_digest() {
        std::uint64_t a = 14695981039346656037ull;
        std::uint64_t b = 0x9e3779b97f4a7c15ull;
        for (const SectionKind kind : kKnownKinds) {
            const SchemaId id = section_schema(kind);
            hash_u64(a, static_cast<std::uint64_t>(kind));
            hash_u64(a, id.a);
            hash_u64(a, id.b);
            hash_u64(b, id.b);
            hash_u64(b, id.a);
            hash_u64(b, static_cast<std::uint64_t>(kind));
        }
        return {a, b};
    }

    inline constexpr SchemaId kHeaderId = header_digest();

    struct SchemaRow {
        SectionKind kind;
        SchemaId id;
    };

    consteval auto make_rows() {
        std::array<SchemaRow, std::size(kKnownKinds)> rows{};
        for (std::size_t i = 0; i < std::size(kKnownKinds); ++i)
            rows[i] = {kKnownKinds[i], section_schema(kKnownKinds[i])};
        return rows;
    }

    inline constexpr auto kRows = make_rows();

} // namespace

SchemaId header_schema_id() {
    return kHeaderId;
}

SchemaId section_schema_id(SectionKind kind) {
    for (const auto& row : kRows) {
        if (row.kind == kind)
            return row.id;
    }
    return {};
}

std::uint64_t section_layout_fingerprint(SectionKind kind) {
    return section_schema_id(kind).a;
}

} // namespace aura::bus
