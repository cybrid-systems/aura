// Wave A3: IROpcode PascalCase (P2996) ↔ display kebab table alignment.

#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

#include "reflect/ir_pod_reflect.hh"
#include "reflect/opcode_display_names.hh"
#include "reflect/opcode_reflect.hh"

namespace {

// PascalCase identifier → kebab-case display name.
// CastOp is the sole historical exception (display "cast", not "cast-op").
constexpr std::string_view pascal_to_display(std::string_view ident, char* buf, std::size_t cap) {
    if (ident == "CastOp")
        return "cast";
    std::size_t n = 0;
    for (std::size_t i = 0; i < ident.size() && n + 1 < cap; ++i) {
        unsigned char c = static_cast<unsigned char>(ident[i]);
        if (std::isupper(c) && i > 0) {
            if (n + 1 >= cap)
                break;
            buf[n++] = '-';
        }
        buf[n++] = static_cast<char>(std::tolower(c));
    }
    buf[n] = '\0';
    return std::string_view(buf, n);
}

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

} // namespace

int main() {
    using E = aura::ir_pod::IROpcode;
    using aura::ir::kIrOpcodeDisplayNameCount;
    using aura::ir::kIrOpcodeDisplayNames;

    constexpr auto N = aura::reflect::enum_count<E>();
    static_assert(N == 54);
    static_assert(N == kIrOpcodeDisplayNameCount);
    static_assert(aura::reflect::validate_enum<E>());

    constexpr auto idents = aura::reflect::build_name_table<E>();
    static_assert(idents.size() == N);
    static_assert(idents[0] == "Nop");
    static_assert(idents[5] == "Add");
    static_assert(idents[28] == "CastOp");
    static_assert(idents[53] == "TopCellLoad");

    check(idents.size() == kIrOpcodeDisplayNameCount, "count match");

    char tmp[64];
    for (std::size_t i = 0; i < N; ++i) {
        auto got = pascal_to_display(idents[i], tmp, sizeof(tmp));
        if (got != kIrOpcodeDisplayNames[i]) {
            std::printf("FAIL: [%zu] ident=%.*s → %.*s want %.*s\n", i, (int)idents[i].size(),
                        idents[i].data(), (int)got.size(), got.data(),
                        (int)kIrOpcodeDisplayNames[i].size(), kIrOpcodeDisplayNames[i].data());
            ++g_failed;
        }
        // opcode_name (by value) must match ident table
        auto on = aura::reflect::opcode_name<E>(static_cast<int>(i));
        if (on != idents[i]) {
            std::printf("FAIL: opcode_name(%zu)=%.*s want %.*s\n", i, (int)on.size(), on.data(),
                        (int)idents[i].size(), idents[i].data());
            ++g_failed;
        }
    }

    // Spot-check display table anchors used by kOpcodeInfo
    check(kIrOpcodeDisplayNames[0] == "nop", "display nop");
    check(kIrOpcodeDisplayNames[21] == "make-closure", "display make-closure");
    check(kIrOpcodeDisplayNames[28] == "cast", "display cast");
    check(kIrOpcodeDisplayNames[52] == "guard-shape", "display guard-shape");

    std::printf("test_opcode_info_align_a3: %s (failed=%d)\n", g_failed ? "FAIL" : "PASS",
                g_failed);
    return g_failed == 0 ? 0 : 1;
}
