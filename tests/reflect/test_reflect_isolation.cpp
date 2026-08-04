// Issue #2290: P2996 placement smoke (g++ 16.1.0).
//
// Positive (this TU): no `import std`, -freflection + reflect.hh → OK.
// Negative (do not regress; fail with hard errors, not silent ICE):
//   1) same TU: import std + #include <meta> / reflect.hh (either order)
//   2) import std + import meta-bearing module + use reflect/consteval API
// Not a ban on "any module file": GMF may include <meta> and compile;
// Aura partitions still isolate because they all import std.
// Policy: P2996 bodies in aura-reflect / non-import-std -freflection TUs;
// import-std code uses C ABI or meta-free headers (hygiene_validate.hh).

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include "reflect/opcode_reflect.hh"
#include "reflect/reflect.hh"

namespace {

enum class IsoOp : unsigned char { Nop, Add, Sub, Mul };

struct IsoPod {
    int id = 0;
    bool ok = false;
    std::array<int, 2> xy{};
};

int g_failed = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

} // namespace

int run_test_reflect_isolation() {
    using aura::reflect::enum_count;
    using aura::reflect::opcode_name;
    using aura::reflect::reflect_members;
    using aura::reflect::to_json;
    using aura::reflect::validate_enum;

    static_assert(enum_count<IsoOp>() == 4);
    static_assert(validate_enum<IsoOp>());
    static_assert(opcode_name<IsoOp>(0) == "Nop");
    static_assert(opcode_name<IsoOp>(static_cast<int>(IsoOp::Mul)) == "Mul");

    constexpr auto members = reflect_members<IsoPod>();
    static_assert(members.size() == 3);
    static_assert(members[0].name == "id");
    static_assert(members[2].array_len == 2);

    IsoPod p{.id = 3, .ok = true, .xy = {4, 5}};
    const std::string json = to_json(p);
    check(json.find("\"id\":3") != std::string::npos, "json id");
    check(json.find("\"ok\":true") != std::string::npos, "json ok");
    check(json.find("\"xy\":[4,5]") != std::string::npos, "json xy");
    check(opcode_name<IsoOp>(1) == "Add", "opcode Add");

    std::printf("test_reflect_isolation: %s (failed=%d)\n", g_failed == 0 ? "PASS" : "FAIL",
                g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_reflect_isolation();
}
#endif
