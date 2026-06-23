// src/compiler/aura_jit_bridge.h
//
// C-linkage declarations for the AOT/JIT bridge functions
// defined in `aura_jit_bridge.cpp`. The bridge layer is a
// pure C-ABI surface (called from generated C registration
// code, from host code, and from test_issue_287.cpp), so the
// header is plain `extern "C"` rather than a C++ module.
//
// Functions:
//   - aura_set_aot_defuse_version / aura_get_aot_defuse_version
//     (Issue #243 — runtime mutation epoch at emit time)
//   - aura_set_module_version / aura_get_module_version
//     (Issue #287 — user-facing module version for hot-reload
//      and multi-agent isolation)
//   - aura_reload_aot_module(path, version)
//     (Issue #287 — host-facing hot-reload entry point;
//      scaffold: dlopen + version check + constructor
//      invocation. The version-keyed func_table swap and
//      multi-agent isolation are tracked as follow-ups to
//      #287.)
//
// These declarations are the minimum needed to call the
// bridge from a non-module translation unit (test files,
// generated registration .c, host loaders). The C++ module
// `aura.compiler.aura_jit_bridge` (if it exists) re-exports
// the same names.

#ifndef AURA_COMPILER_AURA_JIT_BRIDGE_H
#define AURA_COMPILER_AURA_JIT_BRIDGE_H

#include <cstdint>

extern "C" {

// #243 — runtime mutation epoch at AOT emit time.
void aura_set_aot_defuse_version(std::uint64_t v);
std::uint64_t aura_get_aot_defuse_version(void);

// #287 — user-facing module version (hot-reload / multi-agent).
void aura_set_module_version(std::uint64_t v);
std::uint64_t aura_get_module_version(void);

// #287 — AOT hot-reload scaffold.
//   path    - path to the new .so/.dylib
//   version - expected aot_emit_version; 0 = trust binary's own
//
// Returns true on a successful load (dlopen OK + version check
// passed). On failure, logs to stderr and returns false. The
// caller is responsible for keeping the old module alive
// during the swap and for the version-keyed func_table remap
// (follow-up to #287).
bool aura_reload_aot_module(const char* path, std::uint64_t version);

} // extern "C"

#endif // AURA_COMPILER_AURA_JIT_BRIDGE_H