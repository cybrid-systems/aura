// primitives_meta.h — Issue #697: Declarative Primitives Extension Kit
// Category constants, skeleton generator helpers, and extension-kit version.
// PrimMeta backfill lives in evaluator_primitives_registry.cpp (module scope).

#ifndef AURA_COMPILER_PRIMITIVES_META_H
#define AURA_COMPILER_PRIMITIVES_META_H

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace aura::compiler {

// Safety flag bits (mirrors PrimMeta.safety_flags in evaluator.ixx).
inline constexpr std::uint8_t kPrimSafetyMutates = 0x01;
inline constexpr std::uint8_t kPrimSafetyIo = 0x02;
inline constexpr std::uint8_t kPrimSafetyFiber = 0x04;

// Issue #926: PrimMeta.perf_tier / security_level constants.
inline constexpr std::uint8_t kPrimPerfUnknown = 0;
inline constexpr std::uint8_t kPrimPerfHot = 1;    // native / iterative hot path
inline constexpr std::uint8_t kPrimPerfNormal = 2; // typical stdlib
inline constexpr std::uint8_t kPrimPerfCold = 3;   // recursive / rare
inline constexpr std::uint8_t kPrimSecUnknown = 0;
inline constexpr std::uint8_t kPrimSecSafe = 1;       // pure / no I/O
inline constexpr std::uint8_t kPrimSecSandboxed = 2;  // quota / capability gated
inline constexpr std::uint8_t kPrimSecPrivileged = 3; // network / LLM / FS

// Domain categories for AI Agent primitive discovery.
inline constexpr std::string_view kPrimCategoryEda = "eda";
inline constexpr std::string_view kPrimCategorySva = "sva";
inline constexpr std::string_view kPrimCategoryVerification = "verification";
inline constexpr std::string_view kPrimCategoryGeneral = "general";
// Issue #1317: first-class rendering domain for Agent discovery.
inline constexpr std::string_view kPrimCategoryRendering = "rendering";

// Issue #697 extension kit version (bumped when schema/contracts change).
// #709: capture contract + fast slot dispatch + registry stats.
// #498: constraint skeleton template + query:primitive-metadata.
inline constexpr int kPrimitivesExtensionKitVersion = 3;

// Agent-facing skeleton bundle returned by primitive:generate-skeleton.
struct PrimitiveSkeleton {
    std::string category;
    std::string spec;
    std::string cpp_lambda;
    std::string test_snippet;
    std::string registration;
};

namespace primitives_meta_detail {

    inline bool contains_ci(std::string_view hay, std::string_view needle) {
        if (needle.empty() || hay.size() < needle.size())
            return false;
        const auto lower = [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
            bool match = true;
            for (std::size_t j = 0; j < needle.size(); ++j) {
                if (lower(hay[i + j]) != lower(needle[j])) {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
        return false;
    }

    inline std::string detect_category(std::string_view desc) {
        if (contains_ci(desc, "coverpoint") || contains_ci(desc, "covergroup"))
            return std::string(kPrimCategorySva);
        if (contains_ci(desc, "property") || contains_ci(desc, "assert") ||
            contains_ci(desc, "sequence") || contains_ci(desc, "sva"))
            return std::string(kPrimCategorySva);
        if (contains_ci(desc, "verification") || contains_ci(desc, "feedback") ||
            contains_ci(desc, "coverage"))
            return std::string(kPrimCategoryVerification);
        if (contains_ci(desc, "interface") || contains_ci(desc, "modport") ||
            contains_ci(desc, "eda") || contains_ci(desc, "hardware"))
            return std::string(kPrimCategoryEda);
        return std::string(kPrimCategoryGeneral);
    }

    inline std::string suggest_primitive_name(std::string_view desc, std::string_view category) {
        // Issue #1968 / sub-layer 4.4: eda:* primitive vertical retired.
        // No eda:* primitives registered any more; suggest_primitive_name
        // returns "" so generate_primitive_skeleton emits an empty skeleton
        // (callers handle empty prim_name as "no suggestion").
        (void)desc;
        (void)category;
        return std::string{};
    }

} // namespace primitives_meta_detail

inline PrimitiveSkeleton generate_primitive_skeleton(std::string_view description) {
    using namespace primitives_meta_detail;
    PrimitiveSkeleton sk;
    sk.category = detect_category(description);
    // Issue #1968 / sub-layer 4.4: eda:* primitive vertical retired.
    // All eda:* branches below removed. suggest_primitive_name now
    // returns "" for any input — callers handle empty prim_name as
    // "no suggestion" and emit an empty PrimitiveSkeleton.
    const auto prim_name = suggest_primitive_name(description, sk.category);
    (void)prim_name;
    return sk;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_PRIMITIVES_META_H