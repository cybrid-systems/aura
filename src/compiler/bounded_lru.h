// bounded_lru.h — Issue #995: shared max-size + eviction helper for maps
// used by SpecJIT / ShapeProfiler / JIT / ADT / IR cache long-running paths.
//
// Not a full order-statistics LRU (O(n) scan for victim). Good enough for
// default caps of a few thousand entries without a second index.
//
#ifndef AURA_COMPILER_BOUNDED_LRU_H
#define AURA_COMPILER_BOUNDED_LRU_H

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace aura::compiler::util {

// Stamp bumped on every access; lower last_used is a better eviction victim.
struct LruStamp {
    std::uint64_t last_used = 0;
};

// Evict oldest entries from an unordered_map until size <= max_entries.
// Requires mapped_type to have a `last_used` (std::uint64_t) field.
template <typename K, typename V, typename Hash = std::hash<K>, typename Eq = std::equal_to<K>>
inline std::size_t evict_until(std::unordered_map<K, V, Hash, Eq>& map, std::size_t max_entries,
                               std::size_t* evicted_out = nullptr) {
    std::size_t evicted = 0;
    while (map.size() > max_entries && !map.empty()) {
        auto victim = map.begin();
        std::uint64_t oldest = victim->second.last_used;
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (it->second.last_used < oldest) {
                oldest = it->second.last_used;
                victim = it;
            }
        }
        map.erase(victim);
        ++evicted;
    }
    if (evicted_out)
        *evicted_out += evicted;
    return evicted;
}

} // namespace aura::compiler::util

#endif // AURA_COMPILER_BOUNDED_LRU_H
