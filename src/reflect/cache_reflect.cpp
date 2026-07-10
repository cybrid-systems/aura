// cache_reflect.cpp — Non-module, compiled with -freflection.
// Implements serialization for CacheHeader via std::meta.

#include "reflect/reflect.hh"
#include <cstdint>
#include <cstddef>

// Mirror of CacheHeader for serialization (avoids module include issues)
struct CacheHeader {
    std::uint64_t node_offset;
    std::uint64_t string_offset;
    std::uint64_t ir_offset;
    std::uint64_t source_mtime;
    std::uint64_t content_hash;
    char magic[8];
    std::uint32_t version;
    std::uint32_t num_nodes;
    std::uint32_t num_strings;
    std::uint32_t num_functions;
    std::uint32_t sig_offset;
    std::uint32_t sig_size;
};
static_assert(sizeof(CacheHeader) == 72, "CacheHeader must be 72 bytes");

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
    // Validate magic
    if (ch->magic[0] != 'A' || ch->magic[1] != 'U' || ch->magic[2] != 'R' || ch->magic[3] != 'A' ||
        ch->magic[4] != 'C' || ch->magic[5] != 'A' || ch->magic[6] != 'C' || ch->magic[7] != 'H') {
        return -1; // bad magic
    }
    // Validate version
    if (ch->version < 1 || ch->version > 5)
        return -2; // bad version
    // Validate node count
    if (ch->num_nodes == 0 || ch->num_nodes > 10000000)
        return -3; // bad node count
    // Validate offsets
    if (ch->node_offset < 64 || ch->node_offset > 100000000ULL)
        return -4; // bad offset
    // Issue #1104: validate remaining header fields (reject wild values).
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
    // sig_offset 0 = absent; otherwise must be past header.
    if (ch->sig_offset != 0 && ch->sig_offset < 64)
        return -9;
    if (ch->sig_size > 100000000U)
        return -10;
    return 0; // valid
}
} // extern "C"
