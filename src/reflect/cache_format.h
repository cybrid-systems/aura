#ifndef AURA_CACHE_FORMAT_H
#define AURA_CACHE_FORMAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CacheHeader {
    uint64_t node_offset;
    uint64_t string_offset;
    uint64_t ir_offset;
    uint64_t source_mtime;
    uint64_t content_hash;
    char     magic[8];
    uint32_t version;
    uint32_t num_nodes;
    uint32_t num_strings;
    uint32_t num_functions;
} CacheHeader;

#ifdef __cplusplus
}
#endif

// C++ only: verify struct size
#ifdef __cplusplus
static_assert(sizeof(CacheHeader) == 64, "CacheHeader must be 64 bytes");
#endif

void cache_serialize_header(const CacheHeader* h, unsigned char* buf, size_t* out_size);
int  cache_deserialize_header(const unsigned char* buf, size_t size, CacheHeader* h);

#endif
