// ffi_addr.hh — named FFI / JIT address conversions (#4936 T2).
//
// These copy an address-sized value between a pointer, a function pointer,
// and an integer cell. They do not allocate, and they do not launder a
// different object. Callers that need std::bit_cast of a pointer to
// uintptr_t without a function-pointer type stay on address_bits.hh.
#ifndef AURA_COMPILER_FFI_ADDR_HH
#define AURA_COMPILER_FFI_ADDR_HH

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aura::compiler {

template <class Fn> [[nodiscard]] inline Fn fn_from_void(void* addr) noexcept {
    static_assert(std::is_pointer_v<Fn>);
    static_assert(std::is_function_v<std::remove_pointer_t<Fn>>);
    return reinterpret_cast<Fn>(addr);
}

template <class Fn> [[nodiscard]] inline void* void_from_fn(Fn fn_ptr) noexcept {
    static_assert(std::is_pointer_v<Fn>);
    static_assert(std::is_function_v<std::remove_pointer_t<Fn>>);
    return reinterpret_cast<void*>(fn_ptr);
}

[[nodiscard]] inline void* ptr_from_i64(std::int64_t cell) noexcept {
    return std::bit_cast<void*>(static_cast<std::uintptr_t>(cell));
}

[[nodiscard]] inline void* ptr_from_bits(std::uintptr_t bits) noexcept {
    return std::bit_cast<void*>(bits);
}

[[nodiscard]] inline std::int64_t i64_from_ptr(const void* addr) noexcept {
    return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(addr));
}

[[nodiscard]] inline std::uintptr_t bits_from_ptr(const void* addr) noexcept {
    return reinterpret_cast<std::uintptr_t>(addr);
}

template <class Fn> [[nodiscard]] inline Fn fn_from_bits(std::uintptr_t bits) noexcept {
    return fn_from_void<Fn>(ptr_from_bits(bits));
}

template <class Fn> [[nodiscard]] inline std::uintptr_t bits_from_fn(Fn fn_ptr) noexcept {
    static_assert(std::is_pointer_v<Fn>);
    static_assert(std::is_function_v<std::remove_pointer_t<Fn>>);
    return reinterpret_cast<std::uintptr_t>(fn_ptr);
}

template <class Fn> [[nodiscard]] inline std::int64_t i64_from_fn(Fn fn_ptr) noexcept {
    static_assert(std::is_pointer_v<Fn>);
    static_assert(std::is_function_v<std::remove_pointer_t<Fn>>);
    return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(fn_ptr));
}

// The JIT loads an atomic version word as a plain uint64_t slot.
// The address is the atomic object; the cast does not copy the value.
[[nodiscard]] inline std::uint64_t* jit_atomic_u64_slot(std::atomic<std::uint64_t>* word) noexcept {
    return reinterpret_cast<std::uint64_t*>(word);
}

// Pointer-sized word at a byte offset inside an FFI buffer. Densify
// rewrites that word; the offset is the caller's layout, not this helper's.
template <class Byte>
[[nodiscard]] inline void** pointer_slot_at(Byte* base, std::size_t offset) noexcept {
    static_assert(sizeof(Byte) == 1);
    return reinterpret_cast<void**>(base + offset);
}

} // namespace aura::compiler

#endif
