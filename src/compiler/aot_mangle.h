// aot_mangle.h — AOT symbol name mangler (Issue #136, #243, #1369)
//
// Pure helper function used by the AOT bridge to generate valid
// C identifiers from Aura function names. Exposed as a header
// so tests can verify behavior without pulling in the entire
// LLVM/AOT pipeline.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace aura::compiler {

// Issue #136: a thorough name-mangler. The previous version
// only replaced @ . - space with _, which is incomplete: special
// chars like ? ! ( ) [ ] < > & * + = / \ | ' " ; , # $ % ^ ~
// ` all need to be replaced for valid C identifiers. We replace
// any non-[A-Za-z0-9_] char with `_`. Then we collapse runs of
// underscores in the MIDDLE of the name (preserving leading /
// trailing underscores, so reserved names like `__top__` are
// kept verbatim).
//
// Issue #243 Phase 1: version suffix support. The mangled name
// includes a `_vN` suffix derived from the Evaluator's
// `defuse_version_`. This lets the AOT binary self-identify
// which mutation epoch it was generated from.
//
// Issue #1369: always append `_vN`, including `_v0` when
// defuse_version is 0. Previously version==0 skipped the suffix,
// so `__top__` (which also skips the disambiguator) carried no
// version info at all — per-function stale detection was impossible
// for the entry point. Always-on `_vN` makes every symbol carry
// an explicit epoch.
//
// Parameters:
//   original        - the Aura function name to mangle
//   disambiguator   - per-function unique counter (skipped for
//                     __top__ which is the canonical entry point)
//   defuse_version  - the Evaluator's defuse_version_ at emit
//                     time. Always appended as `_v<N>`.
//
// Returns:
//   A valid C identifier that's unlikely to collide with other
//   names. The version suffix ensures names from different
//   mutation epochs never collide with each other.
//
// Issue #1640: mangle now stamps EnvFrame version + linear
// ownership state so that AOT-bridged closures whose captured
// env_frames_/linear_state drift after emit cannot accidentally
// match a stale reload target. Defaults preserve the prior 3-arg
// signature (env_frame_version=0 + linear_state=0 produce the
// same suffix as before); callers that have live env frames must
// thread both values through `generate_registration_c`.
inline std::string mangle_aot_name(std::string_view original, std::uint32_t disambiguator,
                                   std::uint64_t defuse_version = 0,
                                   std::uint64_t env_frame_version = 0,
                                   std::uint8_t linear_state = 0) {
    // Step 1: replace any non-alphanumeric with `_`
    std::string out;
    out.reserve(original.size() + 16);
    for (char c : original) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    // Step 2: collapse runs of underscores in the middle, but
    // preserve leading / trailing underscores (so __top__ stays
    // __top__, not _top_).
    std::size_t first_n = 0;
    while (first_n < out.size() && out[first_n] == '_')
        ++first_n;
    std::size_t last_n = out.size();
    while (last_n > first_n && out[last_n - 1] == '_')
        --last_n;
    std::string prefix = out.substr(0, first_n);
    std::string middle = out.substr(first_n, last_n - first_n);
    std::string suffix = out.substr(last_n);
    std::string compact;
    compact.reserve(middle.size());
    bool prev_underscore = false;
    for (char c : middle) {
        if (c == '_') {
            if (!prev_underscore)
                compact.push_back(c);
            prev_underscore = true;
        } else {
            compact.push_back(c);
            prev_underscore = false;
        }
    }
    std::string result = prefix + compact + suffix;
    // Step 3: append disambiguator at the very end (NOT in the
    // middle, after the name part). This is critical for matching
    // the LLVM-side symbol name, which is constructed as
    // `fn_name + "_" + std::to_string(my_id)`. Inserting the
    // disambiguator in the middle (e.g. for `__lambda__` we
    // would produce `__lambda_0__`) doesn't match the LLVM's
    // `__lambda___0`. Appending at the end produces the same
    // string both sides, so the .reg.c reference and the LLVM
    // object symbol line up. The disambiguator is skipped for
    // `__top__` (the canonical entry point) to match the LLVM
    // side's special case.
    if (original != "__top__") {
        result += "_";
        result += std::to_string(disambiguator);
    }
    // Issue #1369: always append `_vN` (including `_v0`).
    // Format `_v<N>` prevents collision with names that end in digits.
    result += "_v";
    result += std::to_string(defuse_version);
    // Issue #1640: append env_frame_version + linear_state so
    // the mangled name carries enough context for the reload
    // path to detect captured-env drift. Format `_e<N>_l<N>`
    // (e = env_frame_version, l = linear_state) keeps the
    // suffix parseable via aot_parse_version_suffix (which only
    // cares about the trailing `_v<N>`) while exposing the new
    // fields to human-readable diff tools + the bump-aot-env-
    // frame-version-drift-prevented counter on reload.
    if (env_frame_version != 0 || linear_state != 0) {
        result += "_e";
        result += std::to_string(env_frame_version);
        result += "_l";
        result += std::to_string(static_cast<unsigned>(linear_state));
    }
    return result;
}

// Issue #1369: ELF link name for registration / LLVM object.
// `__top__` must remain the exact symbol `runtime.c` main() calls.
// All other functions use the versioned mangle identity.
inline std::string aot_link_name(const std::string& original, std::uint32_t disambiguator,
                                 std::uint64_t defuse_version = 0) {
    if (original == "__top__")
        return "__top__";
    return mangle_aot_name(original, disambiguator, defuse_version);
}

// Issue #1369: parse trailing `_vN` from a mangled name.
// Returns true and writes *out_version on success.
inline bool aot_parse_version_suffix(std::string_view mangled, std::uint64_t* out_version) {
    if (!out_version)
        return false;
    // Find last "_v" followed by digits to end.
    if (mangled.size() < 3)
        return false;
    auto pos = mangled.rfind("_v");
    if (pos == std::string_view::npos || pos + 2 >= mangled.size())
        return false;
    std::string_view digits = mangled.substr(pos + 2);
    if (digits.empty())
        return false;
    for (char c : digits) {
        if (c < '0' || c > '9')
            return false;
    }
    // strtoull needs a C string; digits are trailing so ok if we copy.
    std::string tmp(digits);
    char* end = nullptr;
    unsigned long long v = std::strtoull(tmp.c_str(), &end, 10);
    if (!end || *end != '\0')
        return false;
    *out_version = static_cast<std::uint64_t>(v);
    return true;
}

inline bool aot_mangle_has_version_suffix(std::string_view mangled) {
    std::uint64_t v = 0;
    return aot_parse_version_suffix(mangled, &v);
}

// Host-side stale check: expected version vs version embedded in mangled name.
// Demonstrable without dlopen (unit tests / pre-emit validation).
inline bool aot_mangle_version_is_stale(std::string_view mangled, std::uint64_t expected) {
    std::uint64_t got = 0;
    if (!aot_parse_version_suffix(mangled, &got))
        return true; // missing version → treat as stale under #1369
    return got != expected;
}

} // namespace aura::compiler
