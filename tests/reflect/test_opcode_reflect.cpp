// Issue #2289: GCC 16.1 reflection workaround cleanup.
//
// Exercises opcode_reflect.hh natural P2996 APIs (operator[], extract,
// exact-size tables) and a small to_json / reflect_members smoke so
// cleanup does not regress existing reflection paths.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
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

struct VecHolder {
    std::vector<std::uint32_t> xs;
    std::vector<int> ys;
    std::uint32_t tail = 0;
};

struct SpanHolder {
    std::span<const std::uint32_t> xs;
};

std::uint32_t load_u32(const std::vector<char>& bytes, std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

void check_pod_vectors() {
    VecHolder held;
    held.xs = {0x01020304u, 0xA0B0C0D0u, 0xffffffffu};
    held.ys = {-1, 0x7fffffff};
    held.tail = 0xAABBCCDDu;
    const auto bytes = aura::reflect::auto_serialize(held);
    check(load_u32(bytes, 0) == 12, "member uint32 vector prefix is the byte count");
    check(load_u32(bytes, 16) == 8, "member int vector prefix is the byte count");
    const auto back = aura::reflect::auto_deserialize<VecHolder>(bytes);
    check(back.xs == held.xs, "uint32 elements match, including high bytes");
    check(back.ys == held.ys, "int elements match, including the high bit");
    check(back.tail == held.tail, "tail after both vectors");

    VecHolder empty;
    const auto empty_bytes = aura::reflect::auto_serialize(empty);
    check(load_u32(empty_bytes, 0) == 0 && load_u32(empty_bytes, 4) == 0, "empty vectors prefix 0");
    const auto empty_back = aura::reflect::auto_deserialize<VecHolder>(empty_bytes);
    check(empty_back.xs.empty() && empty_back.ys.empty() && empty_back.tail == 0,
          "empty vectors roundtrip");

    VecHolder one;
    one.xs = {0xffffffffu};
    one.tail = 7;
    const auto one_back =
        aura::reflect::auto_deserialize<VecHolder>(aura::reflect::auto_serialize(one));
    check(one_back.xs.size() == 1 && one_back.xs[0] == 0xffffffffu && one_back.ys.empty() &&
              one_back.tail == 7,
          "single high element and empty sibling");

    const std::vector<std::uint32_t> top{0x01020304u, 0xA0B0C0D0u};
    const auto top_bytes = aura::reflect::auto_serialize(top);
    check(load_u32(top_bytes, 0) == 2, "top-level vector prefix stays the element count");
    check(aura::reflect::auto_deserialize<std::vector<std::uint32_t>>(top_bytes) == top,
          "top-level high bytes");

    std::vector<char> odd(4);
    const std::uint32_t two = 2;
    std::memcpy(odd.data(), &two, 4);
    const auto odd_back = aura::reflect::auto_deserialize<VecHolder>(odd);
    check(odd_back.xs.empty(), "byte count not a multiple of the element leaves the vector empty");

    std::vector<char> trunc(6, '\0');
    const std::uint32_t eight = 8;
    std::memcpy(trunc.data(), &eight, 4);
    const auto trunc_back = aura::reflect::auto_deserialize<VecHolder>(trunc);
    check(trunc_back.xs.empty(), "truncated payload leaves the vector empty");

    const std::uint32_t raw[3] = {0x01020304u, 0xA0B0C0D0u, 0xffffffffu};
    SpanHolder span_held{std::span<const std::uint32_t>(raw, 3)};
    const auto span_bytes = aura::reflect::auto_serialize(span_held);
    check(load_u32(span_bytes, 0) == 12, "span member prefix is the byte count");
    const auto span_back = aura::reflect::auto_deserialize<SpanHolder>(span_bytes);
    check(span_back.xs.size() == 3 && span_back.xs[0] == raw[0] && span_back.xs[1] == raw[1] &&
              span_back.xs[2] == raw[2],
          "span elements match, including high bytes");
}

} // namespace

int run_test_opcode_reflect() {
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
    const auto bytes = aura::reflect::auto_serialize(s);
    std::size_t pos = 0;
    const Smoke back = aura::reflect::auto_deserialize<Smoke>(bytes, pos);
    check(pos == bytes.size(), "serialize consumed the buffer");
    check(back.id == 7 && back.flag && back.name == "x", "struct id/flag/name roundtrip");
    check(back.dims == std::array<int, 3>{1, 2, 3}, "struct array roundtrip");
    check(back.tags.size() == 2 && back.tags[0] == 9 && back.tags[1] == 8,
          "struct vector<int> roundtrip");

    const std::vector<std::uint32_t> nums{1u, 2u, 0x01020304u};
    const auto num_bytes = aura::reflect::auto_serialize(nums);
    const auto nums_back = aura::reflect::auto_deserialize<std::vector<std::uint32_t>>(num_bytes);
    check(nums_back == nums, "vector<uint32_t> roundtrip");

    const std::optional<int> present = 7;
    const std::optional<int> absent;
    check(aura::reflect::auto_deserialize<std::optional<int>>(
              aura::reflect::auto_serialize(present)) == present,
          "optional present");
    check(
        !aura::reflect::auto_deserialize<std::optional<int>>(aura::reflect::auto_serialize(absent))
             .has_value(),
        "optional empty");

    check_pod_vectors();

    const std::string json = to_json(s);
    check(json.find("\"id\":7") != std::string::npos, "json id");
    check(json.find("\"flag\":true") != std::string::npos, "json flag");
    check(json.find("\"name\":\"x\"") != std::string::npos, "json name");
    check(json.find("\"dims\":[1,2,3]") != std::string::npos, "json dims");
    check(json.find("\"tags\":[9,8]") != std::string::npos, "json tags");

    std::printf("test_opcode_reflect: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_opcode_reflect();
}
#endif
