// ci_build_info.h — compile-time + runtime CI reproducibility metadata
// (Issue #675). Included by evaluator_primitives_observability.cpp for
// (query:ci-reproducibility-stats).

#ifndef AURA_COMPILER_CI_BUILD_INFO_H
#define AURA_COMPILER_CI_BUILD_INFO_H

#include <cstdint>
#include <cstdlib>
#include <string>

namespace aura::ci {

inline const char* sanitizer_mode() noexcept {
#if defined(__SANITIZE_ADDRESS__)
    return "asan";
#elif defined(__SANITIZE_THREAD__)
    return "tsan";
#elif defined(__SANITIZE_UNDEFINED__)
    return "ubsan";
#else
    return "none";
#endif
}

inline std::int64_t source_date_epoch() noexcept {
    if (const char* raw = std::getenv("SOURCE_DATE_EPOCH")) {
        char* end = nullptr;
        const auto v = std::strtoll(raw, &end, 10);
        if (end != raw && *end == '\0' && v >= 0)
            return v;
    }
    return 0;
}

inline bool reproducible_flags_active() noexcept {
    return source_date_epoch() > 0;
}

inline const char* build_type_from_env() noexcept {
    if (const char* bt = std::getenv("AURA_BUILD_TYPE"))
        return bt;
    return "unknown";
}

inline bool ccache_disabled() noexcept {
    if (const char* v = std::getenv("CCACHE_DISABLE"))
        return v[0] == '1' && v[1] == '\0';
    return false;
}

} // namespace aura::ci

#endif // AURA_COMPILER_CI_BUILD_INFO_H