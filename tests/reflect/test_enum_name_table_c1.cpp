// Wave C1: generic enum_name_table API across several domain enums.

#include <cstdio>
#include <string_view>

#include "reflect/enum_name_table.hh"
#include "reflect/enum_pods.hh"
#include "reflect/ir_pod_reflect.hh"
#include "reflect/opcode_reflect.hh"

namespace {

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

} // namespace

int main() {
    using namespace aura::reflect;
    using namespace aura::reflect::enum_pods;

    // ── SyntaxMarker (0..2 dense) ────────────────────────────
    {
        static_assert(enum_count<SyntaxMarker>() == 3);
        static_assert(validate_enum<SyntaxMarker>());
        static_assert(enum_name(SyntaxMarker::User) == "User");
        static_assert(enum_name(SyntaxMarker::MacroIntroduced) == "MacroIntroduced");
        static_assert(enum_name(SyntaxMarker::BoolLiteral) == "BoolLiteral");
        static_assert(enum_name<SyntaxMarker>(99) == "<unknown>");
        check(enum_name(SyntaxMarker::User) == "User", "SyntaxMarker User");
        check(enum_name_dense<SyntaxMarker>(1) == "MacroIntroduced", "SyntaxMarker dense");
        constexpr auto list = list_enumerators<SyntaxMarker>();
        check(list.size() == 3 && list[2] == "BoolLiteral", "SyntaxMarker list");
    }

    // ── Region ───────────────────────────────────────────────
    {
        static_assert(enum_count<Region>() == 3);
        static_assert(enum_name(Region::Performance) == "Performance");
        check(enum_name(Region::Evolution) == "Evolution", "Region Evolution");
    }

    // ── ComputeKind ──────────────────────────────────────────
    {
        static_assert(enum_count<ComputeKind>() == 2);
        static_assert(enum_name(ComputeKind::Known) == "Known");
        check(enum_name(ComputeKind::Unknown) == "Unknown", "ComputeKind Unknown");
    }

    // ── IROpcodeClass ────────────────────────────────────────
    {
        static_assert(enum_count<IROpcodeClass>() >= 10);
        static_assert(enum_name(IROpcodeClass::Arith) == "Arith");
        check(enum_name(IROpcodeClass::Guard) == "Guard", "IROpcodeClass Guard");
        check(enum_name_at<IROpcodeClass>(0) == "Nop", "declaration order Nop");
    }

    // ── AuraErrorKind sample ─────────────────────────────────
    {
        static_assert(enum_count<AuraErrorKindSample>() == 5);
        static_assert(enum_name(AuraErrorKindSample::ParseError) == "ParseError");
        check(enum_name(AuraErrorKindSample::UnboundVariable) == "UnboundVariable", "error kind");
    }

    // ── IROpcode via historical opcode_reflect aliases ───────
    {
        using E = aura::ir_pod::IROpcode;
        static_assert(enum_count<E>() == 54);
        check(opcode_name<E>(5) == "Add", "opcode_name alias Add");
        check(enum_name(E::TopCellLoad) == "TopCellLoad", "enum_name TopCellLoad");
        // dense == extent for sequential 0..53
        check(enum_name_dense<E>(0) == "Nop", "dense Nop");
        constexpr auto names = list_opcodes<E>();
        check(names.size() == 54 && names[0] == "Nop", "list_opcodes alias");
    }

    // ── extent table for sparse-style values ─────────────────
    {
        // SyntaxMarker max is 2 → extent 3
        static_assert(enum_max_value<SyntaxMarker>() == 2);
        constexpr auto ext = build_name_table_extent<SyntaxMarker>();
        static_assert(ext.size() == 3);
        static_assert(ext[0] == "User");
        check(ext[1] == "MacroIntroduced", "extent table");
    }

    std::printf("test_enum_name_table_c1: %s (failed=%d)\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed == 0 ? 0 : 1;
}
