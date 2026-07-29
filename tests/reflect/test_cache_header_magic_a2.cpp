// Wave A2: CacheHeader::magic[8] round-trips via auto_serialize;
// wire size matches sizeof(CacheHeader) for mmap load path.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "reflect/cache_format.h"
#include "reflect/reflect.hh"

int main() {
    int failed = 0;
    auto check = [&](bool c, const char* m) {
        if (!c) {
            ++failed;
            std::printf("FAIL: %s\n", m);
        }
    };

    constexpr auto members = aura::reflect::reflect_members<CacheHeader>();
    static_assert(members.size() == 12);
    check(members[5].name == "magic", "name");
    check(members[5].kind == aura::reflect::MemberKind::Array, "kind Array");
    check(members[5].array_len == 8 && members[5].elem_size == 1, "8 chars");

    CacheHeader in{};
    std::memcpy(in.magic, "AURACACH", 8);
    in.version = 5;
    in.num_nodes = 42;
    in.num_strings = 3;
    in.num_functions = 2;
    in.node_offset = sizeof(CacheHeader);
    in.string_offset = 1000;
    in.ir_offset = 2000;
    in.source_mtime = 123456;
    in.content_hash = 0xdeadbeefULL;
    in.sig_offset = 0;
    in.sig_size = 0;

    unsigned char buf[128];
    std::size_t n = 0;
    cache_serialize_header(&in, buf, &n);
    check(n == sizeof(CacheHeader), "serialize size 72");

    CacheHeader out{};
    check(cache_deserialize_header(buf, n, &out) == 1, "deserialize ok");
    check(std::memcmp(out.magic, "AURACACH", 8) == 0, "magic bytes");
    check(out.version == 5 && out.num_nodes == 42, "version/nodes");
    check(out.node_offset == sizeof(CacheHeader), "node_offset");
    check(out.content_hash == 0xdeadbeefULL, "content_hash");
    check(cache_validate_header(&out) == 0, "validate out");

    // mmap-style: first 72 bytes as POD must match struct layout
    CacheHeader pod{};
    std::memcpy(&pod, buf, sizeof(pod));
    check(std::memcmp(pod.magic, "AURACACH", 8) == 0, "pod overlay magic");
    check(pod.version == 5 && pod.num_nodes == 42, "pod overlay fields");
    check(cache_validate_header(&pod) == 0, "validate pod overlay");

    std::printf("test_cache_header_magic_a2: %s (failed=%d)\n", failed ? "FAIL" : "PASS", failed);
    return failed == 0 ? 0 : 1;
}
