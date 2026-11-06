// address_bits.hh — Issue #4936 audited pointer/integer boundary.
//
// Two different operations live here, and they are not interchangeable:
//
// * address_bits / pointer_from_address_bits copy the object representation
//   between a pointer and an integer of the same width (std::bit_cast).
//   Callers use that as a hash, a map key, or an FFI address that was
//   already an integer. Nothing writes through the result.
//
// * moving_pointer_slot is the address of a raw pointer object. The moving
//   collector stores a new pointer at that address. The argument must be a
//   pointer to a non-const type, so a const pointee cannot be registered by
//   accident. EnvFrame::pool_ stays spelled at the call site: #4144 pins
//   the const_cast there.
#ifndef AURA_CORE_ADDRESS_BITS_HH
#define AURA_CORE_ADDRESS_BITS_HH

#include <bit>
#include <cstdint>
#include <type_traits>

namespace aura::core {

[[nodiscard]] inline std::uintptr_t address_bits(const void* ptr) noexcept {
    static_assert(sizeof(const void*) == sizeof(std::uintptr_t));
    return std::bit_cast<std::uintptr_t>(ptr);
}

template <class P> [[nodiscard]] inline P pointer_from_address_bits(std::uintptr_t bits) noexcept {
    static_assert(std::is_pointer_v<P>);
    static_assert(sizeof(P) == sizeof(std::uintptr_t));
    return std::bit_cast<P>(bits);
}

template <class T> [[nodiscard]] inline void** moving_pointer_slot(T*& slot) noexcept {
    static_assert(!std::is_const_v<T>);
    return reinterpret_cast<void**>(&slot);
}

} // namespace aura::core

#endif
