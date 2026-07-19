// @category: integration
// @reason: Issue #587 — primitives-ai-native-stats development slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_587_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:primitives-ai-native-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::string hash_str(aura::compiler::CompilerService& cs, std::string_view hash_expr,
                            std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_expr, key));
    if (!r || !aura::compiler::types::is_string(*r))
        return {};
    const auto idx = aura::compiler::types::as_string_idx(*r);
    const auto& heap = cs.evaluator().string_heap();
    if (idx >= heap.size())
        return {};
    return heap[idx];
}

} // namespace aura_issue_587_detail

int main() {
    using namespace aura_issue_587_detail;

    std::println("=== Issue #587: primitives-ai-native-stats development ===");

    aura::compiler::CompilerService cs;

    // AC1: query:primitives-ai-native-stats returns hash with #587 fields
    {
        std::println("\n--- AC1: query:primitives-ai-native-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:primitives-ai-native-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:primitives-ai-native-stats returns hash");
        CHECK(hash_int(cs, "registry-slots") > 0, "registry-slots > 0");
        CHECK(hash_int(cs, "schema-documented") >= 0, "schema-documented present");
        CHECK(hash_int(cs, "documented-meta") >= 0, "documented-meta present");
        CHECK(hash_int(cs, "skeleton-generations") >= 0, "skeleton-generations present");
        CHECK(hash_int(cs, "introspection-hits") >= 0, "introspection-hits present");
        CHECK(hash_int(cs, "describe-calls") >= 0, "describe-calls present");
        CHECK(hash_int(cs, "list-meta-calls") >= 0, "list-meta-calls present");
        CHECK(hash_int(cs, "meta-coverage-pct") >= 0, "meta-coverage-pct present");
        CHECK(hash_int(cs, "ai-native-hits") >= 1, "ai-native-hits >= 1 after query");
        CHECK(hash_int(cs, "extension-kit-version") == 3, "extension-kit-version == 3");
        CHECK(hash_int(cs, "ai-native-schema") == 587, "ai-native-schema == 587");
        CHECK(hash_int(cs, "primitives-ai-native-total") > 0, "primitives-ai-native-total > 0");
        CHECK(hash_int(cs, "primitives-ai-native-recommendation") >= 0,
              "primitives-ai-native-recommendation present");
    }

    const auto sk_before = hash_int(cs, "skeleton-generations");
    const auto intro_before = hash_int(cs, "introspection-hits");
    const auto describe_before = hash_int(cs, "describe-calls");
    const auto total_before = hash_int(cs, "primitives-ai-native-total");

    // AC2: Agent skeleton generation via DEFINE_PRIMITIVE_META template
    {
        std::println("\n--- AC2: generate-primitive-skeleton ---");
        auto sk = cs.eval("(query:generate-primitive-skeleton "
                          "\"description: weaken SV property for coverage\")");
        CHECK(sk && aura::compiler::types::is_hash(*sk),
              "generate-primitive-skeleton returns hash");
        const auto reg = hash_str(cs,
                                  "(query:generate-primitive-skeleton "
                                  "\"description: weaken SV property for coverage\")",
                                  "registration");
        CHECK(reg.find("DEFINE_PRIMITIVE_META") != std::string::npos,
              "registration contains DEFINE_PRIMITIVE_META");
        const auto sk_after = hash_int(cs, "skeleton-generations");
        CHECK(sk_after > sk_before,
              std::format("skeleton-generations grew ({} -> {})", sk_before, sk_after));
    }

    // AC3: introspection + describe Agent workflow
    {
        std::println("\n--- AC3: meta-catalog + primitive:describe ---");
        (void)cs.eval("(engine:metrics \"query:primitives-meta-catalog\")");
        (void)cs.eval("(primitive:describe \"eda:update-constraint\")");
        const auto intro_after = hash_int(cs, "introspection-hits");
        const auto describe_after = hash_int(cs, "describe-calls");
        CHECK(intro_after > intro_before,
              std::format("introspection-hits grew ({} -> {})", intro_before, intro_after));
        CHECK(describe_after > describe_before,
              std::format("describe-calls grew ({} -> {})", describe_before, describe_after));
        const auto total_after = hash_int(cs, "primitives-ai-native-total");
        CHECK(total_after >= total_before,
              std::format("primitives-ai-native-total monotonic ({} -> {})", total_before,
                          total_after));
        CHECK(hash_int(cs, "meta-coverage-pct") >= 0,
              "meta-coverage-pct readable after Agent workflow");
    }

    // AC4: related AI-native primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto metadata = cs.eval("(engine:metrics \"query:primitive-metadata\")");
        auto meta_stats = cs.eval("(engine:metrics \"query:primitive-meta-stats\")");
        auto catalog = cs.eval("(engine:metrics \"query:primitives-meta-catalog\")");
        auto extension = cs.eval("(engine:metrics \"query:primitives-extension-stats\")");
        CHECK(metadata && aura::compiler::types::is_hash(*metadata),
              "query:primitive-metadata hash regression (#498)");
        CHECK(meta_stats && aura::compiler::types::is_int(*meta_stats),
              "query:primitive-meta-stats int regression (#480)");
        CHECK(catalog && aura::compiler::types::is_hash(*catalog),
              "query:primitives-meta-catalog hash regression (#617)");
        CHECK(extension && aura::compiler::types::is_hash(*extension),
              "query:primitives-extension-stats hash regression (#697)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 152,
              "stats:count >= 152");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}