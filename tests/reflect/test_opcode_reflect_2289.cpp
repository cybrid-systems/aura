// Issue #2289: GCC 16.1 reflection workaround cleanup.
//
// Exercises opcode_reflect.hh natural P2996 APIs (operator[], extract,
// exact-size tables) and a small to_json / reflect_members smoke so
// cleanup does not regress existing reflection paths.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "reflect/opcode_reflect.hh"
#include "reflect/reflect.hh"

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond, const char* msg) {
    if (cond) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

// Mirror IROpcode shape (sequential 0..N-1) without pulling modules.
enum class ProbeOpcode : std::uint8_t {
    Nop,
    ConstI64,
    ConstF64,
    Local,
    Arg,
    Add,
    Sub,
    Mul,
    Div,
    Eq,
    Lt,
    Gt,
    Le,
    Ge,
    And,
    Or,
    Not,
    Branch,
    Jump,
    Call,
    Return,
    MakeClosure,
    Capture,
    CaptureRef,
    Apply,
    NewCell,
    CellSet,
    CellGet,
    CastOp,
    ConstString,
    PrimCall,
    Primitive,
    ConstBool,
    ConstVoid,
    MakePair,
    Car,
    Cdr,
    Raise,
    IsError,
    TryBegin,
    TryEnd,
    HashRef,
    HashSet,
    HashRemove,
    LinearWrap,
    MoveOp,
    BorrowOp,
    MutBorrowOp,
    DropOp,
    RefCountOp,
    ArenaPush,
    ArenaPop,
    GuardShape,
    TopCellLoad,
};

struct Smoke {
    int id = 0;
    bool flag = false;
    std::string name;
    std::array<int, 3> dims{};
    std::vector<int> tags;
};

} // namespace

int run_test_opcode_reflect_2289() {
    using aura::reflect::build_name_table;
    using aura::reflect::enum_count;
    using aura::reflect::list_opcodes;
    using aura::reflect::opcode_name;
    using aura::reflect::reflect_members;
    using aura::reflect::to_json;
    using aura::reflect::validate_enum;

    constexpr auto N = enum_count<ProbeOpcode>();
    static_assert(N == 54, "ProbeOpcode must match kIROpcodeCount shape");
    static_assert(validate_enum<ProbeOpcode>(), "duplicate enum names");

    constexpr auto table = build_name_table<ProbeOpcode>();
    static_assert(table.size() == N);
    static_assert(table[0] == "Nop");
    static_assert(table[static_cast<std::size_t>(ProbeOpcode::Add)] == "Add");
    static_assert(table[static_cast<std::size_t>(ProbeOpcode::TopCellLoad)] == "TopCellLoad");

    check(opcode_name<ProbeOpcode>(0) == "Nop", "opcode_name(0)");
    check(opcode_name<ProbeOpcode>(static_cast<int>(ProbeOpcode::Add)) == "Add",
          "opcode_name(Add)");
    check(opcode_name<ProbeOpcode>(static_cast<int>(ProbeOpcode::TopCellLoad)) == "TopCellLoad",
          "opcode_name(TopCellLoad)");
    check(opcode_name<ProbeOpcode>(-1) == "<unknown>", "opcode_name(-1)");
    check(opcode_name<ProbeOpcode>(static_cast<int>(N)) == "<unknown>",
          "opcode_name(out of range)");

    constexpr auto names = list_opcodes<ProbeOpcode>();
    static_assert(names.size() == N);
    check(names[0] == "Nop", "list_opcodes[0]");
    check(names[names.size() - 1] == "TopCellLoad", "list_opcodes last");

    // reflect_members + to_json still correct after args[i] cleanup
    constexpr auto members = reflect_members<Smoke>();
    static_assert(members.size() == 5);
    check(members[0].name == "id", "member id");
    check(members[3].name == "dims", "member dims");
    check(members[3].array_len == 3, "array_len dims");
    check(members[4].name == "tags", "member tags");

    Smoke s{.id = 7, .flag = true, .name = "x", .dims = {1, 2, 3}, .tags = {9, 8}};
    const std::string json = to_json(s);
    check(json.find("\"id\":7") != std::string::npos, "json id");
    check(json.find("\"flag\":true") != std::string::npos, "json flag");
    check(json.find("\"name\":\"x\"") != std::string::npos, "json name");
    check(json.find("\"dims\":[1,2,3]") != std::string::npos, "json dims");
    check(json.find("\"tags\":[9,8]") != std::string::npos, "json tags");

    std::printf("test_opcode_reflect_2289: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_opcode_reflect_2289();
}
#endif
