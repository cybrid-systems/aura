// security_capabilities.h — capability names for Issue #676 sandbox model.

#ifndef AURA_COMPILER_SECURITY_CAPABILITIES_H
#define AURA_COMPILER_SECURITY_CAPABILITIES_H

#include <cstdint>

extern "C" std::uint64_t aura_fiber_current_id();

namespace aura::compiler::security {

inline constexpr const char* kCapWildcard = "*";
inline constexpr const char* kCapMutate = "mutate";
inline constexpr const char* kCapIo = "io";
inline constexpr const char* kCapIoRead = "io-read";
inline constexpr const char* kCapIoWrite = "io-write";
inline constexpr const char* kCapExec = "exec";

}  // namespace aura::compiler::security

#endif  // AURA_COMPILER_SECURITY_CAPABILITIES_H