// Wave B1: NodeTag P2996 identifiers ↔ kNodeTagNames alignment.

#include <array>
#include <cstdio>
#include <cstdint>
#include <meta>
#include <string_view>

#include "reflect/node_tag_names.hh"
#include "reflect/node_tag_pod.hh"
#include "reflect/opcode_reflect.hh"

namespace {

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

template <typename E, std::size_t N> consteval auto enumerator_value_names() {
    auto en = std::meta::enumerators_of(^^E);
    std::array<std::pair<std::uint32_t, std::string_view>, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i].first = static_cast<std::uint32_t>(std::meta::extract<E>(en[i]));
        out[i].second = std::meta::identifier_of(en[i]);
    }
    return out;
}

} // namespace

int main() {
    using E = aura::ast_pod::NodeTag;
    using aura::ast::kNodeTagGapIndex;
    using aura::ast::kNodeTagNameCount;
    using aura::ast::kNodeTagNames;

    constexpr auto N = aura::reflect::enum_count<E>();
    static_assert(N == aura::ast_pod::kNodeTagEnumeratorCount);
    static_assert(kNodeTagNameCount == 35);
    static_assert(kNodeTagNames[kNodeTagGapIndex] == "<gap>");
    static_assert(kNodeTagNames[0] == "LiteralInt");
    static_assert(kNodeTagNames[13] == "MacroDef");
    static_assert(kNodeTagNames[18] == "DefineType");
    static_assert(kNodeTagNames[34] == "Class");
    static_assert(aura::reflect::validate_enum<E>());

    constexpr auto pairs = enumerator_value_names<E, N>();

    check(N == 34, "enumerator count 34");

    for (std::size_t i = 0; i < N; ++i) {
        auto v = pairs[i].first;
        auto n = pairs[i].second;
        if (v < 1 || v > aura::ast_pod::kNodeTagMax) {
            std::printf("FAIL: value out of range %u for %.*s\n", v, (int)n.size(), n.data());
            ++g_failed;
            continue;
        }
        if (v == 0x0C) {
            check(false, "0x0C must not be an enumerator");
            continue;
        }
        auto idx = static_cast<std::size_t>(v) - 1;
        if (kNodeTagNames[idx] != n) {
            std::printf("FAIL: tag 0x%02x ident=%.*s table=%.*s\n", v, (int)n.size(), n.data(),
                        (int)kNodeTagNames[idx].size(), kNodeTagNames[idx].data());
            ++g_failed;
        }
    }

    check(kNodeTagNames[kNodeTagGapIndex] == "<gap>", "gap name");
    check(kNodeTagNames[static_cast<std::size_t>(E::Call) - 1] == "Call", "Call");
    check(kNodeTagNames[static_cast<std::size_t>(E::MacroDef) - 1] == "MacroDef", "MacroDef");
    check(kNodeTagNames[static_cast<std::size_t>(E::DefineType) - 1] == "DefineType", "DefineType");
    check(kNodeTagNames[static_cast<std::size_t>(E::Class) - 1] == "Class", "Class");

    std::printf("test_node_tag_align_b1: %s (failed=%d)\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed == 0 ? 0 : 1;
}
