// aot_mangle.h — AOT symbol name mangler (Issue #136)
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
// Parameters:
//   original       - the Aura function name to mangle
//   disambiguator  - per-function unique counter (skipped for
//                    __top__ which is the canonical entry point)
//
// Returns:
//   A valid C identifier that's unlikely to collide with other
//   names.
inline std::string mangle_aot_name(const std::string& original,
                                    std::uint32_t disambiguator) {
    // Step 1: replace any non-alphanumeric with `_`
    std::string out;
    out.reserve(original.size() + 16);
    for (char c : original) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    // Step 2: collapse runs of underscores in the middle, but
    // preserve leading / trailing underscores (so __top__ stays
    // __top__, not _top_).
    std::size_t first_n = 0;
    while (first_n < out.size() && out[first_n] == '_') ++first_n;
    std::size_t last_n = out.size();
    while (last_n > first_n && out[last_n - 1] == '_') --last_n;
    std::string prefix = out.substr(0, first_n);
    std::string middle = out.substr(first_n, last_n - first_n);
    std::string suffix = out.substr(last_n);
    std::string compact;
    compact.reserve(middle.size());
    bool prev_underscore = false;
    for (char c : middle) {
        if (c == '_') {
            if (!prev_underscore) compact.push_back(c);
            prev_underscore = true;
        } else {
            compact.push_back(c);
            prev_underscore = false;
        }
    }
    // Step 3: append disambiguator (skipped for __top__ which is
    // the canonical entry point).
    if (original != "__top__") {
        compact += "_";
        compact += std::to_string(disambiguator);
    }
    return prefix + compact + suffix;
}

}  // namespace aura::compiler
