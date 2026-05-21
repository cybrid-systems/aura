// cache_reflect.cpp — Non-module, compiled with -freflection.
// Implements C-linkage serialization for CacheHeader via std::meta.
// Called from cache_impl.cpp (module build unit).

#include "reflect/reflect.hh"
#include "cache_format.h"

void cache_serialize_header(const CacheHeader* h, unsigned char* buf, size_t* out_size) {
    auto vec = aura::reflect::auto_serialize(*h);
    *out_size = vec.size();
    for (size_t i = 0; i < vec.size() && i < 128; ++i)
        buf[i] = static_cast<unsigned char>(vec[i]);
}

int cache_deserialize_header(const unsigned char* buf, size_t size, CacheHeader* h) {
    std::vector<char> vec(buf, buf + size);
    *h = aura::reflect::auto_deserialize<CacheHeader>(vec);
    return 1;
}
