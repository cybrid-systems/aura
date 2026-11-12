// cache_reflect.cpp — Non-module, -freflection (aura-reflect).
// Phase 4 (#2291) + Wave A2: CacheHeader via auto_serialize /
// auto_deserialize / auto_validate. C array magic[8] is MemberKind::Array
// so wire size == sizeof(CacheHeader) (72) and matches mmap load.

#include "reflect/reflect.hh"
#include "reflect/cache_format.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

static_assert(sizeof(CacheHeader) == 72, "CacheHeader must be 72 bytes");
static_assert(aura::reflect::member_count<CacheHeader>() == 12);

extern "C" {

void cache_serialize_header(const void* h, unsigned char* buf, std::size_t* out_size) {
    const CacheHeader* ch = static_cast<const CacheHeader*>(h);
    auto vec = aura::reflect::auto_serialize(*ch);
    *out_size = vec.size();
    for (std::size_t i = 0; i < vec.size() && i < 128; ++i)
        buf[i] = static_cast<unsigned char>(vec[i]);
}

int cache_deserialize_header(const unsigned char* buf, std::size_t size, void* h) {
    CacheHeader* ch = static_cast<CacheHeader*>(h);
    std::vector<char> vec(buf, buf + size);
    *ch = aura::reflect::auto_deserialize<CacheHeader>(vec);
    return 1;
}

namespace {

    bool span_inside(std::uint64_t offset, std::uint64_t length, std::uint64_t file_size) {
        if (length > file_size)
            return false;
        if (offset > file_size - length)
            return false;
        return true;
    }

    bool u64_add(std::uint64_t left, std::uint64_t right, std::uint64_t* out) {
        if (left > UINT64_MAX - right)
            return false;
        *out = left + right;
        return true;
    }

    bool u64_pad8(std::uint64_t value, std::uint64_t* out) {
        if (value > UINT64_MAX - 7)
            return false;
        *out = (value + 7) & ~std::uint64_t{7};
        return true;
    }

    // Smallest node-column footprint the writer emits for node_count nodes
    // with empty child and param payloads. A forged num_nodes that cannot
    // fit is rejected before any column pointer is formed.
    bool node_columns_fit(const CacheHeader& header, std::uint64_t file_size) {
        const std::uint64_t node_count = header.num_nodes;
        std::uint64_t need = 0;
        std::uint64_t part = 0;
        if (!u64_pad8(node_count, &part) || !u64_add(need, part, &need))
            return false;
        if (node_count > UINT64_MAX / 8)
            return false;
        if (!u64_pad8(node_count * 8, &part) || !u64_add(need, part, &need))
            return false;
        if (node_count > UINT64_MAX / 4)
            return false;
        if (!u64_pad8(node_count * 4, &part))
            return false;
        for (int col = 0; col < 7; ++col) {
            if (!u64_add(need, part, &need))
                return false;
        }
        return span_inside(header.node_offset, need, file_size);
    }

} // namespace

int cache_validate_header(const void* h, std::uint64_t file_size) {
    const CacheHeader* ch = static_cast<const CacheHeader*>(h);
    // Structural bounds via reflection (vector/string size gates).
    std::string err;
    if (!aura::reflect::auto_validate(*ch, &err))
        return -11;
    // Semantic checks (domain rules — not representable as pure layout).
    if (ch->magic[0] != 'A' || ch->magic[1] != 'U' || ch->magic[2] != 'R' || ch->magic[3] != 'A' ||
        ch->magic[4] != 'C' || ch->magic[5] != 'A' || ch->magic[6] != 'C' || ch->magic[7] != 'H') {
        return -1;
    }
    if (ch->version < 1 || ch->version > 5)
        return -2;
    if (ch->num_nodes == 0 || ch->num_nodes > 10000000)
        return -3;
    if (ch->node_offset < 64 || ch->node_offset > 100000000ULL)
        return -4;
    if (ch->num_strings > 10000000)
        return -5;
    if (ch->num_functions > 1000000)
        return -6;
    if (ch->string_offset > 0 && ch->string_offset < 64)
        return -7;
    if (ch->string_offset > 100000000ULL)
        return -7;
    if (ch->ir_offset > 100000000ULL)
        return -8;
    if (ch->sig_offset != 0 && ch->sig_offset < 64)
        return -9;
    if (ch->sig_size > 100000000U)
        return -10;
    if (file_size == 0)
        return 0;
    if (file_size < sizeof(CacheHeader))
        return -12;
    if (!node_columns_fit(*ch, file_size))
        return -12;
    if (ch->num_strings > 0 && ch->string_offset == 0)
        return -12;
    if (ch->string_offset != 0) {
        if (ch->num_strings > (UINT64_MAX - 4) / 4)
            return -12;
        const std::uint64_t table = 4 + static_cast<std::uint64_t>(ch->num_strings) * 4;
        if (!span_inside(ch->string_offset, table, file_size))
            return -12;
    }
    if (ch->num_functions > 0) {
        if (ch->ir_offset < sizeof(CacheHeader))
            return -12;
        if (!span_inside(ch->ir_offset, 4, file_size))
            return -12;
    }
    if (ch->sig_offset != 0) {
        std::uint64_t sig_span = 0;
        if (!u64_add(4, ch->sig_size, &sig_span))
            return -12;
        if (!span_inside(ch->sig_offset, sig_span, file_size))
            return -12;
    }
    return 0;
}
} // extern "C"
