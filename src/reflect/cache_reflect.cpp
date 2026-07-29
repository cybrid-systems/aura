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

int cache_validate_header(const void* h) {
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
    return 0;
}
} // extern "C"
