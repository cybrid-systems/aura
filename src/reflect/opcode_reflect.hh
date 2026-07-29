// ──────────────────────────────────────────────────────────────
//  opcode_reflect.hh — P2996 reflection for IROpcode (and friends)
//
//  Wave C1: implementation lives in enum_name_table.hh. This
//  header keeps historical names (opcode_name, list_opcodes, …)
//  as thin aliases so existing call sites need no change.
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_OPCODE_REFLECT_HH
#define AURA_REFLECT_OPCODE_REFLECT_HH

#include "reflect/enum_name_table.hh"

namespace aura::reflect {

// Historical aliases (IROpcode / sequential dense enums).
template <typename E> constexpr std::string_view opcode_name(int value) {
    return enum_name_dense<E>(value);
}

template <typename E> consteval auto list_opcodes() {
    return list_enumerators<E>();
}

// enum_count / validate_enum / build_name_table already in enum_name_table.hh

} // namespace aura::reflect

#endif // AURA_REFLECT_OPCODE_REFLECT_HH
