// @category: unit
// @reason: Issue #2395 — StableNodeRef wire multi-byte fields are
// little-endian (portable); no host-endian memcpy for id/gen/mid/….
//
//   AC1: round-trip serialize → deserialize recovers full ref
//   AC2: wire bytes match known LE golden pattern for multi-byte fields
//   AC3: existing v1/v2 paths still work (hand-built LE v1 blob)
//   AC4: forced byte-swap of multi-byte lanes corrupts recovery (proves
//        we do not "accidentally" depend on native memcpy symmetry alone)
//   AC5: source-cite + CMake + build.py gate

#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.core.ast;
import aura.core.arena;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static FlatAST::StableNodeRef make_ref() {
    FlatAST::StableNodeRef r{};
    r.id = 0x04030201u; // LE wire: 01 02 03 04
    r.gen = 0x0807;     // LE: 07 08
    r.mutation_id_at_capture = 0x0C0B0A0908070605ull;
    r.workspace_id = 0x14131211u;
    r.fiber_id = 0x1C1B1A19u;
    r.last_validated_generation = 0x1615;
    r.wrap_epoch = 0x24232221u;
    r.subtree_gen_at_capture = 0x0E0D;
    r.cow_epoch_at_capture = 0x2C2B2A2928272625ull;
    r.boundary_pinned = true;
    r.tenant_id = 0x3433323130292827ull;
    return r;
}

// ── AC1: round-trip ──
static void ac1_roundtrip() {
    std::println("\n--- #2395 AC1: serialize/deserialize round-trip ---");
    aura::ast::ASTArena arena;
    FlatAST flat(arena.allocator());
    auto in = make_ref();
    std::uint8_t buf[FlatAST::kStableRefSerializedSizeV2]{};
    const auto n = flat.serialize_stable_ref(in, buf);
    CHECK(n == FlatAST::kStableRefSerializedSizeV2, "AC1: wrote 56 bytes");

    FlatAST::StableNodeRef out{};
    CHECK(flat.deserialize_stable_ref(std::span<const std::uint8_t>(buf, n), out),
          "AC1: deserialize ok");
    CHECK(out.id == in.id, "AC1: id");
    CHECK(out.gen == in.gen, "AC1: gen");
    CHECK(out.mutation_id_at_capture == in.mutation_id_at_capture, "AC1: mutation_id");
    CHECK(out.workspace_id == in.workspace_id, "AC1: workspace_id");
    CHECK(out.fiber_id == in.fiber_id, "AC1: fiber_id");
    CHECK(static_cast<std::uint16_t>(out.last_validated_generation) ==
              static_cast<std::uint16_t>(in.last_validated_generation),
          "AC1: last_validated");
    CHECK(out.wrap_epoch == in.wrap_epoch, "AC1: wrap_epoch");
    CHECK(out.subtree_gen_at_capture == in.subtree_gen_at_capture, "AC1: subtree_gen");
    CHECK(out.cow_epoch_at_capture == in.cow_epoch_at_capture, "AC1: cow_epoch");
    CHECK(out.tenant_id == in.tenant_id, "AC1: tenant_id");
    CHECK(out.boundary_pinned == in.boundary_pinned, "AC1: pin");
}

// ── AC2: golden little-endian wire bytes ──
static void ac2_golden_le_bytes() {
    std::println("\n--- #2395 AC2: multi-byte fields are little-endian on wire ---");
    aura::ast::ASTArena arena;
    FlatAST flat(arena.allocator());
    auto in = make_ref();
    std::uint8_t buf[FlatAST::kStableRefSerializedSizeV2]{};
    (void)flat.serialize_stable_ref(in, buf);

    // magic 0x2901A17A LE: 7A A1 01 29
    CHECK(buf[0] == 0x7A && buf[1] == 0xA1 && buf[2] == 0x01 && buf[3] == 0x29,
          "AC2: magic LE bytes");
    // id 0x04030201 LE: 01 02 03 04
    CHECK(buf[4] == 0x01 && buf[5] == 0x02 && buf[6] == 0x03 && buf[7] == 0x04, "AC2: id LE bytes");
    // gen 0x0807 LE: 07 08
    CHECK(buf[8] == 0x07 && buf[9] == 0x08, "AC2: gen LE bytes");
    // mid_lo 0x08070605 LE: 05 06 07 08
    CHECK(buf[12] == 0x05 && buf[13] == 0x06 && buf[14] == 0x07 && buf[15] == 0x08,
          "AC2: mid_lo LE bytes");
    // subtree 0x0E0D LE: 0D 0E
    CHECK(buf[16] == 0x0D && buf[17] == 0x0E, "AC2: subtree_gen LE");
    // last_validated 0x1615 LE: 15 16
    CHECK(buf[18] == 0x15 && buf[19] == 0x16, "AC2: last_validated LE");
    // workspace 0x14131211 LE: 11 12 13 14
    CHECK(buf[20] == 0x11 && buf[21] == 0x12 && buf[22] == 0x13 && buf[23] == 0x14,
          "AC2: workspace_id LE");
    // mid_hi 0x0C0B0A09 LE: 09 0A 0B 0C
    CHECK(buf[24] == 0x09 && buf[25] == 0x0A && buf[26] == 0x0B && buf[27] == 0x0C,
          "AC2: mid_hi LE");
    // fiber 0x1C1B1A19 LE
    CHECK(buf[28] == 0x19 && buf[29] == 0x1A && buf[30] == 0x1B && buf[31] == 0x1C,
          "AC2: fiber_id LE");
    // tenant low 32 of 0x3433323130292827: 27 28 29 30
    CHECK(buf[32] == 0x27 && buf[33] == 0x28 && buf[34] == 0x29 && buf[35] == 0x30,
          "AC2: tenant_id low LE");
    // wrap 0x24232221 LE
    CHECK(buf[40] == 0x21 && buf[41] == 0x22 && buf[42] == 0x23 && buf[43] == 0x24,
          "AC2: wrap_epoch LE");
    std::println("  golden LE bytes verified for multi-byte lanes");
}

// ── AC3: hand-built LE v1 blob still accepted ──
static void ac3_hand_built_v1_le() {
    std::println("\n--- #2395 AC3: hand-built LE v1 blob ---");
    aura::ast::ASTArena arena;
    FlatAST flat(arena.allocator());
    std::array<std::uint8_t, 24> b{};
    // magic LE
    b[0] = 0x7A;
    b[1] = 0xA1;
    b[2] = 0x01;
    b[3] = 0x29;
    // id = 0x0000000B LE
    b[4] = 0x0B;
    b[5] = 0x00;
    b[6] = 0x00;
    b[7] = 0x00;
    // gen = 3 LE
    b[8] = 0x03;
    b[9] = 0x00;
    b[10] = 0; // v1
    b[11] = 0;
    // mid_lo = 0xDEADBEEF LE: EF BE AD DE
    b[12] = 0xEF;
    b[13] = 0xBE;
    b[14] = 0xAD;
    b[15] = 0xDE;
    // subtree = 1
    b[16] = 0x01;
    b[17] = 0x00;
    // reserved / last_validated
    b[18] = 0;
    b[19] = 0;
    // workspace = 0
    FlatAST::StableNodeRef out{};
    CHECK(flat.deserialize_stable_ref(std::span<const std::uint8_t>(b.data(), b.size()), out),
          "AC3: deserialize LE v1 ok");
    CHECK(out.id == 11, "AC3: id");
    CHECK(out.gen == 3, "AC3: gen");
    CHECK(out.mutation_id_at_capture == 0xDEADBEEFull, "AC3: mid_lo");
    CHECK(out.tenant_id == 0, "AC3: tenant default 0");
}

// ── AC4: byte-swap multi-byte lanes → deserialize does not recover ──
static void ac4_forced_swap_corrupts() {
    std::println("\n--- #2395 AC4: forced multi-byte swap corrupts recovery ---");
    aura::ast::ASTArena arena;
    FlatAST flat(arena.allocator());
    auto in = make_ref();
    std::uint8_t buf[FlatAST::kStableRefSerializedSizeV2]{};
    (void)flat.serialize_stable_ref(in, buf);

    // Swap bytes within each multi-byte field (simulates wrong-endian store).
    auto swap_n = [](std::uint8_t* p, std::size_t n) {
        for (std::size_t i = 0; i < n / 2; ++i)
            std::swap(p[i], p[n - 1 - i]);
    };
    // Keep magic correct so deserialize still accepts the buffer; only
    // payload multi-byte lanes are swapped (proves LE decode of non-LE payload).
    swap_n(buf + 4, 4);  // id
    swap_n(buf + 8, 2);  // gen
    swap_n(buf + 12, 4); // mid_lo
    swap_n(buf + 16, 2);
    swap_n(buf + 18, 2);
    swap_n(buf + 20, 4);
    swap_n(buf + 24, 4);
    swap_n(buf + 28, 4);
    swap_n(buf + 32, 8);
    swap_n(buf + 40, 4);
    swap_n(buf + 44, 8);

    FlatAST::StableNodeRef out{};
    CHECK(flat.deserialize_stable_ref(
              std::span<const std::uint8_t>(buf, FlatAST::kStableRefSerializedSizeV2), out),
          "AC4: still parses (magic intact)");
    // At least one multi-byte field must differ from original.
    const bool corrupted = out.id != in.id || out.gen != in.gen ||
                           out.mutation_id_at_capture != in.mutation_id_at_capture ||
                           out.tenant_id != in.tenant_id || out.fiber_id != in.fiber_id;
    CHECK(corrupted, "AC4: swapped multi-byte lanes do not recover original ref");
    std::println("  id in={:08x} out={:08x}", in.id, out.id);
}

// ── AC5: source-cite + gate ──
static void ac5_source_and_gate() {
    std::println("\n--- #2395 AC5: source-cite + gate ---");
    const auto stab = read_file("src/core/ast_stability.cpp");
    const auto ixx = read_file("src/core/ast.ixx");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto linter = read_file("scripts/check_stable_ref_wire_endian_2395.py");

    CHECK(stab.find("Issue #2395") != std::string::npos, "AC5: cites #2395");
    CHECK(stab.find("write_u32_le") != std::string::npos, "AC5: write_u32_le");
    CHECK(stab.find("read_u32_le") != std::string::npos, "AC5: read_u32_le");
    CHECK(stab.find("little-endian") != std::string::npos ||
              stab.find("little endian") != std::string::npos ||
              stab.find("write_u16_le") != std::string::npos,
          "AC5: LE helpers");
    // No raw memcpy of multi-byte ref fields in serialize body (magic was already LE).
    CHECK(stab.find("std::memcpy(out + 4, &ref.id") == std::string::npos,
          "AC5: no host memcpy of ref.id");
    CHECK(ixx.find("little-endian") != std::string::npos || ixx.find("2395") != std::string::npos,
          "AC5: layout docs LE / #2395");
    CHECK(cmake.find("test_stable_ref_wire_endian_2395") != std::string::npos, "AC5: CMake");
    CHECK(build.find("check_stable_ref_wire_endian_2395") != std::string::npos ||
              build.find("cmd_stable_ref_wire_endian_coverage") != std::string::npos,
          "AC5: build.py gate");
    CHECK(!linter.empty(), "AC5: coverage linter present");
}

} // namespace

int main() {
    std::println("=== Issue #2395: StableNodeRef wire little-endian ===");
    ac1_roundtrip();
    ac2_golden_le_bytes();
    ac3_hand_built_v1_le();
    ac4_forced_swap_corrupts();
    ac5_source_and_gate();
    std::println("\n=== #2395 results: passed={} failed={} ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
