// runtime_paths.h — Issue #906: resolve runtime.c / stdlib with AURA_* env.
#ifndef AURA_COMPILER_RUNTIME_PATHS_H
#define AURA_COMPILER_RUNTIME_PATHS_H

#include <cstdlib>
#include <cstddef>
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

// Issue #2772: absolute path of the running aura (or host) binary for
// denseness multi-process re-exec. Prefer /proc/self/exe (Linux), then
// realpath(argv0), then argv0 / AURA_BIN as last resorts.
[[nodiscard]] inline std::string resolve_self_executable(const char* argv0 = nullptr) {
#if defined(__linux__)
    {
        char buf[4096];
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[static_cast<std::size_t>(n)] = '\0';
            return std::string(buf);
        }
    }
#endif
    if (argv0 && argv0[0] != '\0') {
        if (char* rp = ::realpath(argv0, nullptr)) {
            std::string out(rp);
            std::free(rp);
            return out;
        }
        return std::string(argv0);
    }
    if (const char* e = std::getenv("AURA_BIN"); e && e[0] != '\0')
        return std::string(e);
    return {};
}

// Issue #2772: seed process environ AURA_BIN when unset so (getenv
// "AURA_BIN") and child (shell …) inherit the absolute binary path.
// Non-exported shell vars never reach getenv; runners that only assign
// AURA_BIN=… without export hit exit 127 on multi-process denseness.
// Returns the value now in the environment (empty if unresolved).
inline std::string ensure_aura_bin_environ(const char* argv0 = nullptr) {
    if (const char* e = std::getenv("AURA_BIN"); e && e[0] != '\0')
        return std::string(e);
    const std::string self = resolve_self_executable(argv0);
    if (!self.empty())
        ::setenv("AURA_BIN", self.c_str(), /*overwrite=*/0);
    if (const char* e = std::getenv("AURA_BIN"); e && e[0] != '\0')
        return std::string(e);
    return self;
}

} // namespace aura::compiler::paths

#endif
