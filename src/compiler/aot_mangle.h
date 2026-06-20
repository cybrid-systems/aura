// aot_mangle.h — AOT symbol name mangler (Issue #136, #243)
//
// Pure helper function used by the AOT bridge to generate valid
// C identifiers from Aura function names. Exposed as a header
// so tests can verify behavior without pulling in the entire
// LLVM/AOT pipeline.

#pragma once

#include <cstdint>
#include <string>

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
// now includes a `_vN` suffix derived from the Evaluator's
// `defuse_version_`. This lets the AOT binary self-identify
// which mutation epoch it was generated from, so a stale .o
// file (e.g. from a previous Aura session that mutated code
// after a hot-reload) can be detected at runtime before it
// dispatches to an out-of-date function body. The default
// `defuse_version=0` is the "unversioned" baseline; existing
// tests using the no-version path keep working.
//
// Parameters:
//   original        - the Aura function name to mangle
//   disambiguator   - per-function unique counter (skipped for
//                     __top__ which is the canonical entry point)
//   defuse_version  - the Evaluator's defuse_version_ at emit
//                     time. Appended as `_v<N>` to the result.
//                     Defaults to 0 for backward compatibility
//                     with callers that don't track version
//                     (e.g. standalone tests).
//
// Returns:
//   A valid C identifier that's unlikely to collide with other
//   names. The version suffix ensures names from different
//   mutation epochs never collide with each other.
inline std::string mangle_aot_name(const std::string& original, std::uint32_t disambiguator,
                                   std::uint64_t defuse_version = 0) {
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
    // Issue #243 Phase 1: append the defuse_version suffix.
    // The format is `_v<N>` so a mangle result for a function
    // emitted at defuse_version=7 looks like `my_fn_2_v7` —
    // the `_v` prefix prevents accidental collision with names
    // that legitimately end in a digit.
    if (defuse_version != 0) {
        result += "_v";
        result += std::to_string(defuse_version);
    }
    return result;
}

} // namespace aura::compiler
