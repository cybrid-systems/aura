// ──────────────────────────────────────────────────────────────
//  channel.hh — incremental channels over a committed image (#5125)
//
//  Four channels: metrics, CCT, AST patches and worldline results.
//  The coordinator appends records into a section's reserved capacity
//  and advances the section/committed generation; subscribers read only
//  the committed range and ask for the records after their cursor.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_CHANNEL_HH
#define AURA_BUS_CHANNEL_HH

#include "bus/image.hh"

#include <cstdint>
#include <span>
#include <string>

namespace aura::bus {

enum class Channel : std::uint8_t {
    Metrics = 1,
    Cct = 2,
    Patches = 3,
    Worldlines = 4,
};

SectionKind section_for(Channel channel);

struct ChannelView {
    std::uint64_t generation = 0;
    std::uint64_t count = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint32_t element_size = 0;
};

// Reader cursor: the last generation and record count observed.
struct ChannelCursor {
    std::uint64_t generation = 0;
    std::uint64_t count = 0;
};

bool channel_view(std::span<const std::byte> image, Channel channel, ChannelView* out);

// Coordinator: reserve the tail of a channel for bytes; the returned
// offset is where the publisher writes. Fails at capacity (never grows).
bool channel_reserve(std::span<std::byte> image, Channel channel, std::uint64_t bytes,
                     std::uint64_t* offset, std::string* why);

// Coordinator: publish the bytes already written at offset. Validates
// the records, advances length/count/digest/generation and the header's
// committed generation. On failure nothing moves.
bool channel_commit(std::span<std::byte> image, Channel channel, std::uint64_t offset,
                    std::uint64_t bytes, std::uint64_t* generation, std::string* why);

// Subscriber: return the records after the cursor. A cursor that is
// behind by a rebuild, or ahead of the committed count, gets the full
// snapshot (snapshot=true). A stable read is retried if the generation
// changes under it.
bool channel_read(std::span<const std::byte> image, Channel channel, ChannelCursor* cursor,
                  std::span<const std::byte>* records, bool* snapshot, std::string* why);

} // namespace aura::bus

#endif
