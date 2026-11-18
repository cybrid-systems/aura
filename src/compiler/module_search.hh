// Shared (require "…") search path — one rule for the runtime and the LSP.
//
// #5258: the language server resolved a required module only next to the
// requiring file and under the aura.toml root. A program that ran fine with
// AURA_PATH (the release launcher appends it) reported "unbound variable" in
// the editor, and nothing in std/ was reachable. The search order below is
// the runtime's (src/compiler/evaluator_module_loader.cpp); both callers now
// use this function so the editor and `aura main.aura` agree.
#ifndef AURA_COMPILER_MODULE_SEARCH_HH
#define AURA_COMPILER_MODULE_SEARCH_HH

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "core/runtime_config.hh"

namespace aura::module_search {

// One candidate path, with or without the .aura extension. realpath first
// (no stat-then-realpath TOCTOU), then lstat: a symlink to a regular file is
// accepted, a directory or a vanished target is not.
inline std::optional<std::string> try_load(const std::string& full) {
    for (const std::string& candidate : {full, full + ".aura"}) {
        char* real = ::realpath(candidate.c_str(), nullptr);
        if (!real)
            continue;
        std::string out(real);
        ::free(real);
        struct stat lst;
        if (::lstat(out.c_str(), &lst) != 0 || !S_ISREG(lst.st_mode))
            continue;
        return out;
    }
    return std::nullopt;
}

// "a:b::c" -> {a, b, c}; an empty entry (leading/trailing/double colon) is
// skipped, as the runtime does.
inline std::vector<std::string> split_path_list(const char* value) {
    std::vector<std::string> out;
    if (!value)
        return out;
    std::string_view rest(value);
    while (!rest.empty()) {
        const auto colon = rest.find(':');
        const auto dir = rest.substr(0, colon);
        if (!dir.empty())
            out.emplace_back(dir);
        if (colon == std::string_view::npos)
            break;
        rest.remove_prefix(colon + 1);
    }
    return out;
}

// Directory holding the running executable ("" when /proc is unavailable).
// A copied or installed bin/aura finds its own lib/ here; AURA_PATH stays
// first.
inline const std::string& executable_dir() {
    static const std::string dir = [] {
        char buf[4096];
        const auto n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n <= 0)
            return std::string{};
        std::string exe(buf, static_cast<std::size_t>(n));
        const auto slash = exe.rfind('/');
        return slash == std::string::npos ? std::string{} : exe.substr(0, slash);
    }();
    return dir;
}

// Resolve `path` the way the runtime does. `base_dir` is the directory the
// requiring file lives in ("" when it has none). Returns a canonical path.
inline std::optional<std::string> resolve(const std::string& path, std::string_view base_dir) {
    if (!path.empty() && path.front() == '/')
        return try_load(path);

    // #4918: "./x" and "../x" are relative to the requiring file.
    if ((path.starts_with("./") || path.starts_with("../")) && !base_dir.empty()) {
        if (auto hit = try_load(std::string(base_dir) + "/" + path))
            return hit;
    }

    if (char* cwd = ::getcwd(nullptr, 0)) {
        std::string dir(cwd);
        ::free(cwd);
        if (auto hit = try_load(dir + "/" + path))
            return hit;
    }

    for (const auto& dir : split_path_list(::aura::config::raw(::aura::config::Key::AURA_PATH))) {
        if (auto hit = try_load(dir + "/" + path))
            return hit;
    }

    // #4824: executable-relative stdlib discovery, then the historical
    // cwd-relative build/ → lib/ probes.
    if (const auto& exe = executable_dir(); !exe.empty()) {
        for (const char* rel : {"/lib/", "/../lib/"}) {
            if (auto hit = try_load(exe + rel + path))
                return hit;
        }
    }
    if (auto hit = try_load("../lib/" + path))
        return hit;
    if (auto hit = try_load("./lib/" + path))
        return hit;
    return std::nullopt;
}

} // namespace aura::module_search

#endif // AURA_COMPILER_MODULE_SEARCH_HH
