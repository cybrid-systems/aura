// write_scope.hh — Issue #4912: path-scoped Write (`--grant write:out/`, the
// `ci` preset). When a scope is set, every host-path write primitive
// (write-file, make-directory, file-copy, file-delete, serialize-workspace,
// generate-type-sigs) must land under one of its roots; anything else is an
// effect-denied refusal. No scope = Write applies to every path (dev,
// `--grant write`). Set once at startup, before any form runs; read-only
// afterwards.

#ifndef AURA_COMPILER_WRITE_SCOPE_HH
#define AURA_COMPILER_WRITE_SCOPE_HH

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace aura::compiler::security {

struct WriteScope {
    std::vector<std::filesystem::path> roots; // weakly_canonical, absolute
    std::string label;                        // "out/, data/" as the user wrote them
};

inline WriteScope& write_scope_storage() noexcept {
    static WriteScope scope;
    return scope;
}

[[nodiscard]] inline bool write_scope_active() noexcept {
    return !write_scope_storage().roots.empty();
}

[[nodiscard]] inline const std::string& write_scope_label() noexcept {
    return write_scope_storage().label;
}

// Absolute, symlink-resolved (for the part that exists) form of `p`.
[[nodiscard]] inline std::filesystem::path write_scope_canonical(const std::filesystem::path& p) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(p, ec);
    if (ec)
        return {};
    auto canon = std::filesystem::weakly_canonical(abs, ec);
    if (ec)
        return abs.lexically_normal();
    return canon.lexically_normal();
}

// `specs` are directories as given (relative to the current directory).
inline void set_write_scope(const std::vector<std::string>& specs) {
    auto& s = write_scope_storage();
    s.roots.clear();
    s.label.clear();
    for (const auto& spec : specs) {
        if (spec.empty())
            continue;
        auto root = write_scope_canonical(spec);
        if (root.empty())
            continue;
        s.roots.push_back(std::move(root));
        if (!s.label.empty())
            s.label += ", ";
        s.label += spec;
        if (!spec.ends_with('/'))
            s.label += '/';
    }
}

// True when no scope is set or `path` resolves at or under a root.
[[nodiscard]] inline bool path_in_write_scope(std::string_view path) noexcept {
    const auto& s = write_scope_storage();
    if (s.roots.empty())
        return true;
    try {
        const auto target = write_scope_canonical(std::filesystem::path(std::string(path)));
        if (target.empty())
            return false;
        for (const auto& root : s.roots) {
            const auto rel = target.lexically_relative(root);
            if (rel.empty())
                continue;
            const auto first = *rel.begin();
            if (first == ".." || rel.is_absolute())
                continue;
            return true;
        }
    } catch (...) {
    }
    return false;
}

// The refusal text. "effect-denied: write-path …" keeps the #4909
// classification (effect-denied + hint) on every face.
[[nodiscard]] inline std::string write_scope_deny_reason(std::string_view op,
                                                         std::string_view path) {
    return std::format("effect-denied: write-path not granted for {} (writable: {}) op={}", path,
                       write_scope_label(), op);
}

} // namespace aura::compiler::security

#endif
