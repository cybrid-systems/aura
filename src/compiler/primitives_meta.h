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
        if (contains_ci(desc, "constraint"))
            return "eda:update-constraint";
        if (category == kPrimCategorySva && contains_ci(desc, "coverpoint"))
            return "eda:add-coverpoint-bin";
        if (category == kPrimCategorySva && contains_ci(desc, "property"))
            return "eda:weaken-property";
        if (category == kPrimCategoryVerification)
            return "eda:run-verification-feedback";
        if (category == kPrimCategoryEda && contains_ci(desc, "evolution"))
            return "eda:demo-sv-self-evolution";
        if (category == kPrimCategoryEda)
            return "eda:custom-interface-mutate";
        return "eda:custom-mutate";
    }

} // namespace primitives_meta_detail

inline PrimitiveSkeleton generate_primitive_skeleton(std::string_view description) {
    using namespace primitives_meta_detail;
    PrimitiveSkeleton sk;
    sk.category = detect_category(description);
    const auto prim_name = suggest_primitive_name(description, sk.category);

    if (prim_name == "eda:update-constraint") {
        sk.spec = "(constraint-id expr-string) -> bool";
        sk.cpp_lambda =
            "add_mutate(\"eda:update-constraint\", [&ev](const auto& a) -> EvalValue {\n"
            "    bool ok = true;\n"
            "    MutationBoundaryGuard guard(ev, &ok);\n"
            "    ws->append_param(cid, pool->intern(expr));\n"
            "    return make_bool(true);\n"
            "});";
        sk.test_snippet = "(eda:update-constraint <constraint-id> \"val < 128;\")";
        sk.registration = "DEFINE_PRIMITIVE_META(2, false, kPrimSafetyMutates, \"sva\", "
                          "\"Append constraint expr on native Constraint node.\", "
                          "\"(int string) -> bool\")";
    } else if (prim_name == "eda:add-coverpoint-bin") {
        sk.spec = "(coverpoint-id bin-name-string) -> bool";
        sk.cpp_lambda =
            "add_mutate(\"eda:add-coverpoint-bin\", [&ev](const auto& a) -> EvalValue {\n"
            "    bool ok = true;\n"
            "    MutationBoundaryGuard guard(ev, &ok);\n"
            "    ws->append_param(cp_id, pool->intern(bin_name));\n"
            "    return make_bool(true);\n"
            "});";
        sk.test_snippet = "(eda:add-coverpoint-bin <coverpoint-id> \"mid\")";
        sk.registration = "DEFINE_PRIMITIVE_META(2, false, kPrimSafetyMutates, \"sva\", "
                          "\"Add coverpoint bin.\", \"(int string) -> bool\")";
    } else if (prim_name == "eda:weaken-property") {
        sk.spec = "(property-id disable-clause-string) -> bool";
        sk.cpp_lambda = "add_mutate(\"eda:weaken-property\", [&ev](const auto& a) -> EvalValue {\n"
                        "    bool ok = true;\n"
                        "    MutationBoundaryGuard guard(ev, &ok);\n"
                        "    return make_bool(true);\n"
                        "});";
        sk.test_snippet = "(eda:weaken-property <property-id> \"reset\")";
        sk.registration = "DEFINE_PRIMITIVE_META(2, false, kPrimSafetyMutates, \"sva\", "
                          "\"Weaken property.\", \"(int string) -> bool\")";
    } else if (prim_name == "eda:run-verification-feedback") {
        sk.spec = "(report-kind-string report-text-string) -> bool";
        sk.cpp_lambda =
            "add(\"eda:run-verification-feedback\", [&ev](const auto& a) -> EvalValue {\n"
            "    return make_bool(true);\n"
            "});";
        sk.test_snippet = "(eda:run-verification-feedback \"coverage.log\" \"0 hole_a\")";
        sk.registration = "DEFINE_PRIMITIVE_META(2, false, kPrimSafetyMutates|kPrimSafetyFiber, "
                          "\"verification\", \"Feedback loop.\", \"(string string) -> bool\")";
    } else if (prim_name == "eda:demo-sv-self-evolution") {
        sk.spec = "(example-string cycles-int) -> int";
        sk.cpp_lambda = "add(\"eda:demo-sv-self-evolution\", [&ev](const auto& a) -> EvalValue {\n"
                        "    return make_int(successes);\n"
                        "});";
        sk.test_snippet = "(eda:demo-sv-self-evolution \"interface\" 50)";
        sk.registration = "DEFINE_PRIMITIVE_META(2, false, kPrimSafetyMutates|kPrimSafetyFiber, "
                          "\"eda\", \"Self-evolution demo.\", \"(string int) -> int\")";
    } else {
        sk.spec = "(node-id payload-string) -> bool";
        sk.cpp_lambda = "add_mutate(\"eda:custom-mutate\", [&ev](const auto& a) -> EvalValue {\n"
                        "    bool ok = true;\n"
                        "    MutationBoundaryGuard guard(ev, &ok);\n"
                        "    return make_bool(true);\n"
                        "});";
        sk.test_snippet = "(eda:custom-mutate <node-id> \"payload\")";
        sk.registration = "DEFINE_PRIMITIVE_META(2, false, kPrimSafetyMutates, \"eda\", "
                          "\"Custom EDA mutate.\", \"(int string) -> bool\")";
    }
    return sk;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_PRIMITIVES_META_H