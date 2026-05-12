// ──────────────────────────────────────────────────────────────
//  abf_reader_registry.hh — Static NodeTag → reader dispatch
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_ABF_READER_REGISTRY_HH
#define AURA_REFLECT_ABF_READER_REGISTRY_HH

#include <cstdint>
#include <unordered_map>

namespace aura::reflect::abf_registry {

using ReadFn = void* (*)(void* reader, void* obj);

class ReaderRegistry {
public:
    static void reg(std::uint8_t tag, ReadFn fn) { map()[tag] = fn; }
    static ReadFn get(std::uint8_t tag) {
        auto it = map().find(tag);
        return it != map().end() ? it->second : nullptr;
    }
    static std::size_t count() { return map().size(); }
private:
    static std::unordered_map<std::uint8_t, ReadFn>& map() {
        static std::unordered_map<std::uint8_t, ReadFn> m;
        return m;
    }
};

// Helper: wrap a member function into a ReadFn
// Handles any return type (cast to void* via reinterpret_cast)
template <typename T, typename R, auto MemFn>
ReadFn make_reader() {
    return [](void* r, void* obj) -> void* {
        return reinterpret_cast<void*>(
            (static_cast<T*>(obj)->*MemFn)(*static_cast<R*>(r)));
    };
}

// Convenience: register a member fn without repeating types
#define ABF_REGISTER_MEMBER(tag, T, R, fn) \
    ReaderRegistry::reg((tag), make_reader<T, R, &T::fn>())

} // namespace aura::reflect::abf_registry

#endif