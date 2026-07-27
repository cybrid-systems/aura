// @category: unit
// @reason: Issue #2198 — complete StableNodeRef wire format v2
// (tenant / fiber / pin / cow_epoch / wrap / full mutation_id).
//
//   AC1: v2 round-trips tenant_id, fiber_id, boundary_pinned,
//        cow_epoch_at_capture, wrap_epoch (+ full mutation_id)
//   AC2: Old 24-byte (v1) buffers still accepted; missing fields
//        default safely (tenant=0, pin=false, …)
//   AC3: After deserialize, is_valid_in / is_valid_in_layer behave
//        as for live refs (pin + cow, gen match)
//   AC4: Documented layout + magic/version; no silent tenant drop
//   AC5: Tenant preserve (#2056) + pin flag survive serialize path

#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;
import aura.core.arena;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static FlatAST::StableNodeRef make_full_ref() {
    FlatAST::StableNodeRef r{};
    r.id = 42;
    r.gen = 7;
    r.mutation_id_at_capture = 0xAABBCCDDEEFF0011ull;
    r.workspace_id = 3;
    r.fiber_id = 99;
    r.last_validated_generation = 7;
    r.wrap_epoch = 5;
    r.subtree_gen_at_capture = 2;
    r.cow_epoch_at_capture = 0x1122334455667788ull;
    r.boundary_pinned = true;
    r.tenant_id = 0xC0FFEEull;
    return r;
}

// Hand-build a v1 (24-byte) buffer matching pre-#2198 serializer.
static std::array<std::uint8_t, 24> make_v1_blob(NodeId id, std::uint16_t gen, std::uint32_t mid_lo,
                                                 std::uint16_t subtree_gen,
                                                 std::uint32_t workspace_id) {
    std::array<std::uint8_t, 24> b{};
    const std::uint32_t magic = FlatAST::kStableRefMagic;
    std::memcpy(b.data(), &magic, 4);
    std::memcpy(b.data() + 4, &id, 4);
    std::memcpy(b.data() + 8, &gen, 2);
    b[10] = 0; // version v1
    b[11] = 0;
    std::memcpy(b.data() + 12, &mid_lo, 4);
    std::memcpy(b.data() + 16, &subtree_gen, 2);
    b[18] = 0;
    b[19] = 0;
    std::memcpy(b.data() + 20, &workspace_id, 4);
    return b;
}

// ── AC1: v2 full fidelity ───────────────────────────────────
static void ac1_v2_roundtrip() {
    std::println("\n--- AC1: v2 round-trip full provenance ---");
    aura::ast::ASTArena arena;
    FlatAST flat(arena.allocator());
    auto in = make_full_ref();
    std::uint8_t buf[FlatAST::kStableRefSerializedSize];
    const auto n = flat.serialize_stable_ref(in, buf);
    CHECK(n == FlatAST::kStableRefSerializedSizeV2, "serialize writes 56 bytes");
    CHECK(n == FlatAST::kStableRefSerializedSize, "kStableRefSerializedSize == V2");
    CHECK(buf[10] == FlatAST::kStableRefWireVersionV2, "version byte = 2");
    CHECK((buf[11] & FlatAST::kStableRefFlagBoundaryPinned) != 0, "pin flag set on wire");

    FlatAST::StableNodeRef out{};
    CHECK(flat.deserialize_stable_ref(std::span<const std::uint8_t>(buf, n), out),
          "deserialize v2 ok");
    CHECK(out.id == in.id, "id");
    CHECK(out.gen == in.gen, "gen");
    CHECK(out.mutation_id_at_capture == in.mutation_id_at_capture, "full mutation_id (no trunc)");
    CHECK(out.workspace_id == in.workspace_id, "workspace_id");
    CHECK(out.fiber_id == in.fiber_id, "fiber_id");
    CHECK(out.tenant_id == in.tenant_id, "tenant_id");
    CHECK(out.boundary_pinned == in.boundary_pinned, "boundary_pinned");
    CHECK(out.cow_epoch_at_capture == in.cow_epoch_at_capture, "cow_epoch_at_capture");
    CHECK(out.wrap_epoch == in.wrap_epoch, "wrap_epoch");
    CHECK(out.subtree_gen_at_capture == in.subtree_gen_at_capture, "subtree_gen");
    CHECK(out.last_validated_generation == in.last_validated_generation, "last_validated");
}

// ── AC2: v1 backward compat ─────────────────────────────────
static void ac2_v1_compat() {
    std::println("\n--- AC2: v1 24-byte buffers still accepted ---");
    aura::ast::ASTArena arena;
    FlatAST flat(arena.allocator());
    auto v1 = make_v1_blob(/*id=*/11, /*gen=*/3, /*mid_lo=*/0xDEADBEEF, /*subtree=*/1,
                           /*ws=*/0);
    FlatAST::StableNodeRef out{};
    CHECK(flat.deserialize_stable_ref(std::span<const std::uint8_t>(v1.data(), v1.size()), out),
          "deserialize v1 ok");
    CHECK(out.id == 11, "v1 id");
    CHECK(out.gen == 3, "v1 gen");
    CHECK(out.mutation_id_at_capture == 0xDEADBEEFull, "v1 mid low only");
    CHECK(out.workspace_id == 0, "v1 workspace");
    CHECK(out.subtree_gen_at_capture == 1, "v1 subtree_gen");
    // Safe defaults for fields not on v1 wire
    CHECK(out.tenant_id == 0, "v1 tenant default 0");
    CHECK(out.fiber_id == 0, "v1 fiber default 0");
    CHECK(!out.boundary_pinned, "v1 pin default false");
    CHECK(out.wrap_epoch == 0, "v1 wrap default 0");
    CHECK(out.cow_epoch_at_capture == 0, "v1 cow default 0");

    // Too-short buffer rejects
    CHECK(!flat.deserialize_stable_ref(std::span<const std::uint8_t>(v1.data(), 8), out),
          "short buffer rejected");
    // Bad magic rejects
    auto bad = v1;
    bad[0] ^= 0xFF;
    CHECK(!flat.deserialize_stable_ref(std::span<const std::uint8_t>(bad.data(), bad.size()), out),
          "bad magic rejected");
}

// ── AC3: is_valid_in / is_valid_in_layer after restore ──────
static void ac3_validity_after_restore() {
    std::println("\n--- AC3: validity after deserialize ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr && ws->size() > 1, "workspace");

    // Capture a live ref with full provenance stamps.
    FlatAST::StableNodeRef live{};
    live.id = 1;
    for (NodeId id = 1; id < ws->size(); ++id) {
        if (ws->is_live_node(id) && !ws->is_free_slot(id)) {
            live = ws->make_safe_ref(id);
            break;
        }
    }
    CHECK(live.id != NULL_NODE, "captured live ref");
    live.tenant_id = 77;
    live.fiber_id = 5;
    live.boundary_pinned = true;
    live.cow_epoch_at_capture = ws->workspace_cow_epoch();
    live.wrap_epoch = ws->wrap_epoch(); // if accessor exists

    // wrap_epoch may need public accessor — use field from make_safe_ref if set.
    std::uint8_t buf[FlatAST::kStableRefSerializedSize];
    const auto n = ws->serialize_stable_ref(live, buf);
    FlatAST::StableNodeRef restored{};
    CHECK(ws->deserialize_stable_ref(std::span<const std::uint8_t>(buf, n), restored),
          "restore ok");
    CHECK(restored.tenant_id == 77, "tenant survives");
    CHECK(restored.fiber_id == 5, "fiber survives");
    CHECK(restored.boundary_pinned, "pin survives");
    CHECK(restored.is_valid_in(*ws), "is_valid_in after restore");
    // is_valid_in_layer: same workspace, cow match or pin allows.
    CHECK(restored.is_valid_in_layer(*ws, restored.workspace_id), "is_valid_in_layer with pin/cow");
}

// ── AC4: documented layout + no silent tenant drop ──────────
static void ac4_docs_and_no_truncation() {
    std::println("\n--- AC4: layout docs + no silent tenant truncation ---");
    auto h = read_file("src/core/ast.ixx");
    auto impl = read_file("src/core/ast_stability.cpp");
    CHECK(h.find("kStableRefSerializedSizeV2") != std::string::npos, "V2 constant");
    CHECK(h.find("kStableRefSerializedSizeV1") != std::string::npos, "V1 constant");
    CHECK(h.find("kStableRefWireVersionV2") != std::string::npos, "wire version");
    CHECK(h.find("tenant_id") != std::string::npos && h.find("#2198") != std::string::npos,
          "tenant + #2198 in header");
    CHECK(impl.find("never silently dropped") != std::string::npos ||
              impl.find("no silent") != std::string::npos ||
              impl.find("tenant_id") != std::string::npos,
          "impl documents tenant");
    CHECK(impl.find("kStableRefWireVersionV2") != std::string::npos, "writes v2");
    CHECK(FlatAST::kStableRefSerializedSizeV1 == 24, "V1 size 24");
    CHECK(FlatAST::kStableRefSerializedSizeV2 == 56, "V2 size 56");
    CHECK(FlatAST::kStableRefMagic == 0x2901A17Au, "magic stable");

    // High half of mutation_id and non-zero tenant must survive.
    aura::ast::ASTArena arena;
    FlatAST flat(arena.allocator());
    FlatAST::StableNodeRef in{};
    in.id = 1;
    in.gen = 1;
    in.mutation_id_at_capture = (1ull << 40) | 0x1234ull;
    in.tenant_id = 0xFEEDFACEull;
    std::uint8_t buf[FlatAST::kStableRefSerializedSize];
    flat.serialize_stable_ref(in, buf);
    FlatAST::StableNodeRef out{};
    flat.deserialize_stable_ref(
        std::span<const std::uint8_t>(buf, FlatAST::kStableRefSerializedSize), out);
    CHECK(out.mutation_id_at_capture == in.mutation_id_at_capture, "no mid high truncation");
    CHECK(out.tenant_id == in.tenant_id, "no tenant truncation");
}

// ── AC5: tenant + pin path (#2056) ──────────────────────────
static void ac5_tenant_and_pin() {
    std::println("\n--- AC5: tenant preserve + pin flag ---");
    aura::ast::ASTArena arena;
    FlatAST flat(arena.allocator());
    FlatAST::StableNodeRef in{};
    in.id = 9;
    in.gen = 1;
    in.tenant_id = 2056;
    in.boundary_pinned = true;
    in.fiber_id = 12;
    in.cow_epoch_at_capture = 100;
    in.wrap_epoch = 2;

    std::uint8_t buf[FlatAST::kStableRefSerializedSize];
    flat.serialize_stable_ref(in, buf);
    FlatAST::StableNodeRef out{};
    CHECK(flat.deserialize_stable_ref(
              std::span<const std::uint8_t>(buf, FlatAST::kStableRefSerializedSize), out),
          "deser");
    CHECK(out.tenant_id == 2056, "AC5 tenant 2056 preserved");
    CHECK(out.boundary_pinned, "AC5 pin survives");
    CHECK(out.fiber_id == 12, "fiber");
    CHECK(out.cow_epoch_at_capture == 100, "cow");
    CHECK(out.wrap_epoch == 2, "wrap");

    // EDSL serialize with optional tenant/pin args.
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define y 2)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    // (ast:ref-serialize id gen mid ws fiber tenant wrap cow pin)
    auto r = cs.eval("(ast:ref-serialize 1 1 99 0 3 2056 1 10 1)");
    CHECK(r.has_value(), "ast:ref-serialize with provenance args");
    // Round-trip via deserialize still returns 4-tuple (compat shape).
    auto d =
        cs.eval("(let ((s (ast:ref-serialize 1 1 99 0 3 2056 1 10 1))) (ast:ref-deserialize s))");
    CHECK(d.has_value(), "ast:ref-deserialize after v2 serialize");
}

} // namespace

int main() {
    std::println("=== Issue #2198: StableNodeRef wire format v2 ===");
    ac1_v2_roundtrip();
    ac2_v1_compat();
    ac3_validity_after_restore();
    ac4_docs_and_no_truncation();
    ac5_tenant_and_pin();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
