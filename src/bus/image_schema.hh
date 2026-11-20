// ──────────────────────────────────────────────────────────────
//  image_schema.hh — stable schema ids for the bus image (#5122)
//
//  The ids are generated at compile time by P2996 reflection over the
//  canonical structs in image_format.hh. They cover the frozen field
//  prefix only: appending fields keeps the id and bumps the layout
//  version, so independently built tools stay additive-compatible.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_IMAGE_SCHEMA_HH
#define AURA_BUS_IMAGE_SCHEMA_HH

#include "bus/image_format.hh"

#include <cstdint>

namespace aura::bus {

struct SchemaId {
    std::uint64_t a = 0;
    std::uint64_t b = 0;

    friend constexpr bool operator==(const SchemaId&, const SchemaId&) = default;
};

// Schema id over the ordered section catalog and each section's id.
SchemaId header_schema_id();

// Schema id for one section kind; zeros for an unknown kind.
SchemaId section_schema_id(SectionKind kind);

// 64-bit layout fingerprint of one section kind (that section's id.a).
std::uint64_t section_layout_fingerprint(SectionKind kind);

} // namespace aura::bus

#endif
