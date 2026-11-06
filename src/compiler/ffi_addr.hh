// ffi_addr.hh — named FFI / JIT address conversions (#4936 T2).
//
// These copy an address-sized value between a pointer, a function pointer,
// and an integer cell. They do not allocate, and they do not launder a
// different object. Callers that need std::bit_cast of a pointer to
// uintptr_t without a function-pointer type stay on address_bits.hh.
#ifndef AURA_COMPILER_FFI_ADDR_HH
#define AURA_COMPILER_FFI_ADDR_HH

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aura::compiler {

template <class Fn> [[nodiscard]] inline Fn fn_from_void(void* p) noexcept {
    static_assert(std::is_pointer_v<Fn>);
    static_assert(std::is_function_v<std::remove_pointer_t<Fn>>);
    return reinterpret_cast<Fn>(p);
}

template <class Fn> [[nodiscard]] inline void* void_from_fn(Fn fn) noexcept {
    static_assert(std::is_pointer_v<Fn>);
    static_assert(std::is_function_v<std::remove_pointer_t<Fn>>);
    return reinterpret_cast<void*>(fn);
}

[[nodiscard]] inline void* ptr_from_i64(std::int64_t cell) noexcept {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(cell));
}

[[nodiscard]] inline std::int64_t i64_from_ptr(const void* p) noexcept {
    return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(p));
}

[[nodiscard]] inline std::uintptr_t bits_from_ptr(const void* p) noexcept {
    return reinterpret_cast<std::uintptr_t>(p);
}

// Pointer-sized word at a byte offset inside an FFI buffer. Densify
// rewrites that word; the offset is the caller's layout, not this helper's.
template <class Byte> [[nodiscard]] inline void** pointer_slot_at(Byte* base, std::size_t offset) noexcept {
    static_assert(sizeof(Byte) == 1);
    return reinterpret_cast<void**>(base + offset);
}

} // namespace aura::compiler

#endif
