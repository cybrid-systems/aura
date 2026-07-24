// aot_mangle.h — AOT symbol name mangler (Issue #136, #243, #1369, #1640, #2015)
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
//
// Issue #2015: parse side now extracts `_eN_lN` as well as `_vN`
// so probe / stale-check paths can detect captured-env and
// linear-ownership drift (not only defuse epoch).
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
    // (e = env_frame_version, l = linear_state).
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

// Issue #2015: full version suffix components from a mangled name.
// Layout:  ..._v<defuse>[_e<env>_l<linear>]
struct AotVersionSuffix {
    std::uint64_t defuse_version = 0;
    std::uint64_t env_frame_version = 0;
    std::uint8_t linear_state = 0;
    bool has_defuse = false;
    bool has_env_linear = false;
};

// Issue #2015: parse `_vN` and optional trailing `_eN_lN`.
// Returns true when a defuse `_vN` was found (env/linear optional).
inline bool aot_parse_full_version_suffix(std::string_view mangled, AotVersionSuffix* out) {
    if (!out)
        return false;
    *out = {};
    // Find last "_v" followed by at least one digit (defuse stamp).
    // With optional `_eN_lN` after it, digits no longer run to end-of-string.
    auto pos = mangled.rfind("_v");
    if (pos == std::string_view::npos || pos + 2 >= mangled.size())
        return false;
    std::size_t i = pos + 2;
    if (i >= mangled.size() || mangled[i] < '0' || mangled[i] > '9')
        return false;
    std::uint64_t defuse = 0;
    while (i < mangled.size() && mangled[i] >= '0' && mangled[i] <= '9') {
        defuse = defuse * 10ull + static_cast<std::uint64_t>(mangled[i] - '0');
        ++i;
    }
    out->defuse_version = defuse;
    out->has_defuse = true;

    // Optional `_eN_lN` (Issue #1640 emit / #2015 parse).
    if (i + 3 <= mangled.size() && mangled[i] == '_' && mangled[i + 1] == 'e') {
        std::size_t j = i + 2;
        if (j < mangled.size() && mangled[j] >= '0' && mangled[j] <= '9') {
            std::uint64_t env = 0;
            while (j < mangled.size() && mangled[j] >= '0' && mangled[j] <= '9') {
                env = env * 10ull + static_cast<std::uint64_t>(mangled[j] - '0');
                ++j;
            }
            if (j + 2 <= mangled.size() && mangled[j] == '_' && mangled[j + 1] == 'l') {
                std::size_t k = j + 2;
                if (k < mangled.size() && mangled[k] >= '0' && mangled[k] <= '9') {
                    std::uint64_t lin = 0;
                    while (k < mangled.size() && mangled[k] >= '0' && mangled[k] <= '9') {
                        lin = lin * 10ull + static_cast<std::uint64_t>(mangled[k] - '0');
                        ++k;
                    }
                    // Accept only when the env/linear suffix consumes the tail
                    // (no trailing garbage after `_lN`).
                    if (k == mangled.size()) {
                        out->env_frame_version = env;
                        out->linear_state = static_cast<std::uint8_t>(lin > 255ull ? 255ull : lin);
                        out->has_env_linear = true;
                    }
                }
            }
        }
    }
    return true;
}

// Issue #1369 / #2015: parse trailing `_vN` (env/linear ignored for this API).
// Returns true and writes *out_version on success.
// Fixed #2015: allows optional `_eN_lN` after `_vN` (previously required
// digits to run to end-of-string, so `_v7_e5_l1` failed to parse).
inline bool aot_parse_version_suffix(std::string_view mangled, std::uint64_t* out_version) {
    if (!out_version)
        return false;
    AotVersionSuffix full{};
    if (!aot_parse_full_version_suffix(mangled, &full) || !full.has_defuse)
        return false;
    *out_version = full.defuse_version;
    return true;
}

inline bool aot_mangle_has_version_suffix(std::string_view mangled) {
    std::uint64_t v = 0;
    return aot_parse_version_suffix(mangled, &v);
}

// Host-side stale check: expected version vs version embedded in mangled name.
// Demonstrable without dlopen (unit tests / pre-emit validation).
// Issue #2015: optional expected env_frame_version + linear_state.
// When any of (expected_env, expected_linear, mangled `_e/_l`) is active,
// all three components must match; defuse-only names remain backward-compatible
// when both expected env/linear are 0 and the mangled name has no `_e/_l`.
inline bool aot_mangle_version_is_stale(std::string_view mangled, std::uint64_t expected_defuse,
                                        std::uint64_t expected_env_frame = 0,
                                        std::uint8_t expected_linear = 0) {
    AotVersionSuffix got{};
    if (!aot_parse_full_version_suffix(mangled, &got) || !got.has_defuse)
        return true; // missing version → treat as stale under #1369
    if (got.defuse_version != expected_defuse)
        return true;
    const bool host_tracks_env_linear = (expected_env_frame != 0 || expected_linear != 0);
    if (!host_tracks_env_linear && !got.has_env_linear)
        return false; // pure defuse comparison (legacy)
    const std::uint64_t got_env = got.has_env_linear ? got.env_frame_version : 0;
    const std::uint8_t got_lin = got.has_env_linear ? got.linear_state : 0;
    return got_env != expected_env_frame || got_lin != expected_linear;
}

// Issue #2015: which component(s) mismatched (for metrics / diagnostics).
// Returns true if stale; out flags may be null.
inline bool aot_mangle_version_is_stale_detail(std::string_view mangled,
                                               std::uint64_t expected_defuse,
                                               std::uint64_t expected_env_frame,
                                               std::uint8_t expected_linear, bool* out_defuse_stale,
                                               bool* out_env_stale, bool* out_linear_stale) {
    if (out_defuse_stale)
        *out_defuse_stale = false;
    if (out_env_stale)
        *out_env_stale = false;
    if (out_linear_stale)
        *out_linear_stale = false;
    AotVersionSuffix got{};
    if (!aot_parse_full_version_suffix(mangled, &got) || !got.has_defuse) {
        if (out_defuse_stale)
            *out_defuse_stale = true;
        return true;
    }
    bool stale = false;
    if (got.defuse_version != expected_defuse) {
        if (out_defuse_stale)
            *out_defuse_stale = true;
        stale = true;
    }
    const bool host_tracks = (expected_env_frame != 0 || expected_linear != 0);
    if (host_tracks || got.has_env_linear) {
        const std::uint64_t got_env = got.has_env_linear ? got.env_frame_version : 0;
        const std::uint8_t got_lin = got.has_env_linear ? got.linear_state : 0;
        if (got_env != expected_env_frame) {
            if (out_env_stale)
                *out_env_stale = true;
            stale = true;
        }
        if (got_lin != expected_linear) {
            if (out_linear_stale)
                *out_linear_stale = true;
            stale = true;
        }
    }
    return stale;
}

} // namespace aura::compiler
