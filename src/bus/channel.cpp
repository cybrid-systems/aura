// ──────────────────────────────────────────────────────────────
//  channel.cpp — incremental channel commit/read (#5125)
// ──────────────────────────────────────────────────────────────
#include "bus/channel.hh"

#include <atomic>
#include <cstring>

namespace aura::bus {
namespace {

    constexpr std::uint64_t kNoEntry = ~0ull;

    std::uint64_t find_entry_offset(std::span<const std::byte> image, SectionKind kind) {
        const ImageHeader h = load_header(image);
        for (std::uint32_t i = 0; i < h.section_count; ++i) {
            const std::uint64_t at =
                h.section_dir_off + static_cast<std::uint64_t>(i) * sizeof(SectionEntry);
            if (at + sizeof(SectionEntry) > image.size())
                return kNoEntry;
            SectionEntry e{};
            std::memcpy(&e, image.data() + at, sizeof(e));
            if (e.kind == static_cast<std::uint32_t>(kind))
                return at;
        }
        return kNoEntry;
    }

    std::uint64_t fnv_bytes(std::span<const std::byte> image, std::uint64_t off,
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
        std::memcpy(&v, image.data() + off, sizeof(T));
        return v;
    }

    bool validate_records(std::span<const std::byte> image, Channel channel, std::uint64_t offset,
                          std::uint64_t bytes, std::uint64_t prior_count, std::string* why) {
        const auto fail = [&](const char* text) {
            if (why)
                *why = text;
            return false;
        };
        const std::uint64_t element = expected_element_size(section_for(channel));
        if (element == 0 || bytes % element != 0)
            return fail("channel payload is not a whole number of records");
        const std::uint64_t records = bytes / element;
        for (std::uint64_t i = 0; i < records; ++i) {
            const std::uint64_t at = offset + i * element;
            switch (channel) {
                case Channel::Metrics: {
                    const MetricRecord m = load_at<MetricRecord>(image, at);
                    if ((m.value >> 63) != 0)
                        return fail("metric value carries bit 63");
                    break;
                }
                case Channel::Cct: {
                    const CctRecord c = load_at<CctRecord>(image, at);
                    if ((c.mid_lo >> 62) != 0)
                        return fail("CCT mid carries bits above the low 62");
                    if (c.parent != kImageNoParent && c.parent >= prior_count + i)
                        return fail("CCT parent does not point backwards");
                    break;
                }
                case Channel::Patches: {
                    const PatchRecord p = load_at<PatchRecord>(image, at);
                    if ((p.new_value >> 63) != 0)
                        return fail("patch value carries bit 63");
                    break;
                }
                case Channel::Worldlines: {
                    const WorldlineRecord w = load_at<WorldlineRecord>(image, at);
                    if (w.state > static_cast<std::uint8_t>(WorldlineState::Rejected))
                        return fail("worldline state outside the domain");
                    if (w.constraint_result > static_cast<std::uint8_t>(ConstraintResult::Fail))
                        return fail("worldline constraint result outside the domain");
                    if (w.parent != kImageNoParent && w.parent >= prior_count + i)
                        return fail("worldline parent does not point backwards");
                    break;
                }
            }
        }
        return true;
    }

} // namespace

SectionKind section_for(Channel channel) {
    switch (channel) {
        case Channel::Metrics:
            return SectionKind::Metrics;
        case Channel::Cct:
            return SectionKind::Cct;
        case Channel::Patches:
            return SectionKind::Patches;
        case Channel::Worldlines:
            return SectionKind::Worldlines;
    }
    return SectionKind::Metrics;
}

bool channel_view(std::span<const std::byte> image, Channel channel, ChannelView* out) {
    const std::uint64_t at = find_entry_offset(image, section_for(channel));
    if (at == kNoEntry)
        return false;
    SectionEntry e{};
    std::memcpy(&e, image.data() + at, sizeof(e));
    out->generation = e.generation;
    out->count = e.count;
    out->offset = e.offset;
    out->length = e.length;
    out->element_size = e.element_size;
    return true;
}

bool channel_reserve(std::span<std::byte> image, Channel channel, std::uint64_t bytes,
                     std::uint64_t* offset, std::string* why) {
    const std::uint64_t at = find_entry_offset(image, section_for(channel));
    if (at == kNoEntry) {
        if (why)
            *why = "channel section is missing";
        return false;
    }
    SectionEntry e{};
    std::memcpy(&e, image.data() + at, sizeof(e));
    if (bytes == 0) {
        if (why)
            *why = "empty reservation";
        return false;
    }
    if (e.length > e.capacity || bytes > e.capacity - e.length) {
        if (why)
            *why = "channel capacity exhausted";
        return false;
    }
    *offset = e.offset + e.length;
    return true;
}

bool channel_commit(std::span<std::byte> image, Channel channel, std::uint64_t offset,
                    std::uint64_t bytes, std::uint64_t* generation, std::string* why) {
    const std::uint64_t at = find_entry_offset(image, section_for(channel));
    if (at == kNoEntry) {
        if (why)
            *why = "channel section is missing";
        return false;
    }
    SectionEntry e{};
    std::memcpy(&e, image.data() + at, sizeof(e));
    if (offset != e.offset + e.length) {
        if (why)
            *why = "publish offset is not the committed tail";
        return false;
    }
    const std::uint64_t element = expected_element_size(section_for(channel));
    if (bytes == 0 || element == 0 || bytes % element != 0) {
        if (why)
            *why = "publish length is not whole records";
        return false;
    }
    if (e.length > e.capacity || bytes > e.capacity - e.length) {
        if (why)
            *why = "publish exceeds the reserved capacity";
        return false;
    }
    if (!validate_records(image, channel, offset, bytes, e.count, why))
        return false;

    ImageHeader h = load_header(image);
    const std::uint64_t next_gen = h.committed_gen + 2;
    e.length += bytes;
    e.count += static_cast<std::uint32_t>(bytes / element);
    e.digest = fnv_bytes(image, e.offset, e.length);
    e.generation = next_gen;
    std::memcpy(image.data() + at, &e, sizeof(e));
    std::atomic_thread_fence(std::memory_order_release);
    h.committed_gen = next_gen;
    std::memcpy(image.data(), &h, sizeof(h));
    *generation = next_gen;
    return true;
}

bool channel_read(std::span<const std::byte> image, Channel channel, ChannelCursor* cursor,
                  std::span<const std::byte>* records, bool* snapshot, std::string* why) {
    const std::uint64_t at = find_entry_offset(image, section_for(channel));
    if (at == kNoEntry) {
        if (why)
            *why = "channel section is missing";
        return false;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        const ImageHeader h = load_header(image);
        if ((h.committed_gen & 1u) != 0)
            continue;
        SectionEntry e{};
        std::memcpy(&e, image.data() + at, sizeof(e));
        if (e.generation > h.committed_gen)
            continue;
        if (e.length > e.capacity || e.count > kImageMaxRecords) {
            if (why)
                *why = "corrupt channel directory";
            return false;
        }
        const bool rebuild =
            cursor->generation == 0 || cursor->generation > e.generation || cursor->count > e.count;
        const std::uint64_t from_count = rebuild ? 0 : cursor->count;
        const std::uint64_t from = e.offset + from_count * e.element_size;
        const std::uint64_t len = (e.count - from_count) * e.element_size;
        *records = {static_cast<const std::byte*>(image.data()) + from - e.offset + e.offset,
                    static_cast<std::size_t>(len)};
        *snapshot = rebuild;
        cursor->generation = e.generation;
        cursor->count = e.count;
        return true;
    }
    if (why)
        *why = "generation changed while reading";
    return false;
}

} // namespace aura::bus
