// runtime_paths.h — Issue #906: resolve runtime.c / stdlib with AURA_* env.
#ifndef AURA_COMPILER_RUNTIME_PATHS_H
#define AURA_COMPILER_RUNTIME_PATHS_H

#include <cstdlib>
#include <string>
#include <unistd.h>

namespace aura::compiler::paths {

inline constexpr const char* kRuntimeRel = "lib/runtime.c";
inline constexpr const char* kStdlibRel = "lib/std/";
inline constexpr const char* kInstallRuntime = "/usr/local/share/aura/runtime.c";
inline constexpr const char* kEnvRuntimeDir = "AURA_RUNTIME_DIR";
inline constexpr const char* kEnvStdlibDir = "AURA_STDLIB_DIR";

[[nodiscard]] inline bool readable(const std::string& path) noexcept {
    return ::access(path.c_str(), R_OK) == 0;
}

// Resolve path to runtime.c: AURA_RUNTIME_DIR, CWD relatives, install path.
[[nodiscard]] inline std::string resolve_runtime_c() {
    if (const char* dir = std::getenv(kEnvRuntimeDir)) {
        std::string base = dir;
        while (!base.empty() && (base.back() == '/' || base.back() == '\\'))
            base.pop_back();
        if (readable(base + "/runtime.c"))
            return base + "/runtime.c";
        if (readable(base + "/lib/runtime.c"))
            return base + "/lib/runtime.c";
        if (readable(base))
            return base; // may already be a file path
    }
    for (const char* rel : {kRuntimeRel, "../lib/runtime.c", kInstallRuntime}) {
        if (readable(rel))
            return rel;
    }
    return kRuntimeRel; // default for error messages
}

// Resolve stdlib root directory (trailing slash).
[[nodiscard]] inline std::string resolve_stdlib_root() {
    if (const char* dir = std::getenv(kEnvStdlibDir)) {
        std::string base = dir;
        if (!base.empty() && base.back() != '/')
            base.push_back('/');
        return base;
    }
    if (readable("lib/std") || readable("lib/std/INDEX.aura"))
        return kStdlibRel;
    if (readable("../lib/std") || readable("../lib/std/INDEX.aura"))
        return "../lib/std/";
    return kStdlibRel;
}

} // namespace aura::compiler::paths

#endif
