// @category: integration
// @reason: Issue #498 — AI-native primitive metadata + skeleton ergonomics

#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_498_detail {
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

static std::int64_t snap_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:primitive-metadata) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::string hash_str(aura::compiler::CompilerService& cs, const char* hash_expr,
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

} // namespace aura_issue_498_detail

int main() {
    using namespace aura_issue_498_detail;

    std::println("=== Issue #498: AI-native primitive metadata ===");

    aura::compiler::CompilerService cs;

    // AC1: query:primitive-metadata fields
    {
        std::println("\n--- AC1: query:primitive-metadata ---");
        auto stats = cs.eval("(query:primitive-metadata)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:primitive-metadata returns hash");
        CHECK(snap_stat(cs, "registry-slots") > 0, "registry-slots > 0");
        CHECK(snap_stat(cs, "documented-meta") >= 0, "documented-meta present");
        CHECK(snap_stat(cs, "schema-documented") >= 0, "schema-documented present");
        CHECK(snap_stat(cs, "skeleton-generations") >= 0, "skeleton-generations present");
        CHECK(snap_stat(cs, "extension-kit-version") == 3, "extension-kit-version == 3");
        CHECK(snap_stat(cs, "metadata-total") >= 0, "metadata-total present");
    }

    const auto sk_before = snap_stat(cs, "skeleton-generations");

    // AC2: query:generate-primitive-skeleton alias + constraint template
    {
        std::println("\n--- AC2: query:generate-primitive-skeleton ---");
        auto sk = cs.eval("(query:generate-primitive-skeleton "
                          "\"description: update SV constraint expression\")");
        CHECK(sk && aura::compiler::types::is_hash(*sk), "query alias returns hash");
        const auto snippet = hash_str(cs,
                                      "(query:generate-primitive-skeleton "
                                      "\"update constraint on native node\")",
                                      "test-snippet");
        CHECK(snippet.find("eda:update-constraint") != std::string::npos,
              "constraint skeleton references eda:update-constraint");
        const auto reg = hash_str(cs,
                                  "(query:generate-primitive-skeleton "
                                  "\"update constraint on native node\")",
                                  "registration");
        CHECK(reg.find("DEFINE_PRIMITIVE_META") != std::string::npos,
              "registration contains DEFINE_PRIMITIVE_META");
        const auto sk_after = snap_stat(cs, "skeleton-generations");
        CHECK(sk_after > sk_before,
              std::format("skeleton-generations grew ({} -> {})", sk_before, sk_after));
    }

    // AC3: primitive:describe + query:primitive-list-with-meta regression
    {
        std::println("\n--- AC3: describe + list-with-meta regression ---");
        auto desc = cs.eval("(primitive:describe \"eda:update-constraint\")");
        CHECK(desc && aura::compiler::types::is_pair(*desc),
              "primitive:describe eda:update-constraint");
        auto list = cs.eval("(query:primitive-list-with-meta)");
        CHECK(list && aura::compiler::types::is_pair(*list), "primitive-list-with-meta regression");
        CHECK(snap_stat(cs, "describe-calls") >= 1, "describe-calls bumped");
    }

    // AC4: query:primitive-meta-stats regression
    {
        std::println("\n--- AC4: primitive-meta-stats regression ---");
        auto meta = cs.eval("(query:primitive-meta-stats)");
        CHECK(meta && aura::compiler::types::is_int(*meta), "primitive-meta-stats regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 101,
              "stats:count == 101");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}