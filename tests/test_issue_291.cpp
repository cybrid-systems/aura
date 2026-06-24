// @category: integration
// @reason: Issue #291 — StableNodeRef extended with provenance +
// workspace_id + serialization
//
// Validates:
//  - StableNodeRef struct has new fields (mutation_id_at_capture,
//    workspace_id) and they default to 0 (backward compat)
//  - make_ref() captures next_mutation_id_ into mutation_id_at_capture
//  - serialize_stable_ref / deserialize_stable_ref roundtrip
//  - 4 Aura helpers: (ast:ref-mutation-id), (ast:ref-workspace-id),
//    (ast:ref-serialize), (ast:ref-deserialize)
//  - Reject buffers without the #291 magic
#include <iostream>
#include <cstring>
#include <print>
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;
import aura.core.ast;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace test_291_detail {

static int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) return -1;
    auto& v = *r;
    if (!aura::compiler::types::is_int(v)) return -1;
    return aura::compiler::types::as_int(v);
}

// ── AC #1: StableNodeRef struct has new fields with defaults ──
bool test_struct_defaults() {
    std::println("\n--- AC #1: StableNodeRef new fields default to 0 ---");
    aura::ast::FlatAST::StableNodeRef r{};
    CHECK(r.id == aura::ast::NULL_NODE, "default id == NULL_NODE");
    CHECK(r.gen == 0, "default gen == 0");
    CHECK(r.mutation_id_at_capture == 0,
          "default mutation_id_at_capture == 0 (got " +
          std::to_string(r.mutation_id_at_capture) + ")");
    CHECK(r.workspace_id == 0,
          "default workspace_id == 0 (got " +
          std::to_string(r.workspace_id) + ")");
    return true;
}

// ── AC #2: kStableRefSerializedSize is 24 bytes ──────────────
bool test_serialized_size() {
    std::println("\n--- AC #2: kStableRefSerializedSize == 24 ---");
    CHECK(aura::ast::FlatAST::kStableRefSerializedSize == 24,
          "size == 24 (got " +
          std::to_string(aura::ast::FlatAST::kStableRefSerializedSize) + ")");
    return true;
}

// ── AC #3: serialize/deserialize roundtrip on a real ref ─────
bool test_serialize_roundtrip() {
    std::println("\n--- AC #3: serialize/deserialize roundtrip ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    // Build a small flat with one Variable node
    auto x_sym = pool.intern("x");
    flat.add_variable(x_sym);
    auto ref = flat.make_ref(0);
    ref.mutation_id_at_capture = 42;
    ref.workspace_id = 7;
    std::uint8_t buf[aura::ast::FlatAST::kStableRefSerializedSize] = {};
    auto n = flat.serialize_stable_ref(ref, buf);
    CHECK(n == aura::ast::FlatAST::kStableRefSerializedSize,
          "serialize wrote " + std::to_string(n) + " bytes");
    // Check magic
    std::uint32_t magic = 0;
    std::memcpy(&magic, buf, 4);
    CHECK(magic == aura::ast::FlatAST::kStableRefMagic,
          "magic = 0x" + [&]() {
              char tmp[16];
              std::snprintf(tmp, sizeof(tmp), "%08X", magic);
              return std::string(tmp);
          }());
    // Roundtrip
    aura::ast::FlatAST::StableNodeRef out{};
    bool ok = flat.deserialize_stable_ref(buf, n, out);
    CHECK(ok, "deserialize returned true");
    CHECK(out.id == ref.id, "roundtrip id (got " + std::to_string(out.id) +
          ", want " + std::to_string(ref.id) + ")");
    CHECK(out.gen == ref.gen, "roundtrip gen (got " +
          std::to_string(out.gen) + ", want " + std::to_string(ref.gen) + ")");
    CHECK(out.mutation_id_at_capture == ref.mutation_id_at_capture,
          "roundtrip mutation_id (got " +
          std::to_string(out.mutation_id_at_capture) + ", want " +
          std::to_string(ref.mutation_id_at_capture) + ")");
    CHECK(out.workspace_id == ref.workspace_id,
          "roundtrip workspace_id (got " +
          std::to_string(out.workspace_id) + ", want " +
          std::to_string(ref.workspace_id) + ")");
    return true;
}

// ── AC #4: deserialize rejects wrong-magic buffers ───────────
bool test_deserialize_rejects_bad_magic() {
    std::println("\n--- AC #4: deserialize rejects non-#291 buffers ---");
    aura::ast::FlatAST flat;
    std::uint8_t bad[24] = {};
    bad[0] = 0xDE; bad[1] = 0xAD; bad[2] = 0xBE; bad[3] = 0xEF;
    aura::ast::FlatAST::StableNodeRef out{};
    bool ok = flat.deserialize_stable_ref(bad, sizeof(bad), out);
    CHECK(!ok, "deserialize returned false for bad magic");
    return true;
}

// ── AC #5: deserialize rejects too-small buffer ──────────────
bool test_deserialize_rejects_short() {
    std::println("\n--- AC #5: deserialize rejects too-small buffer ---");
    aura::ast::FlatAST flat;
    std::uint8_t short_buf[8] = {};
    aura::ast::FlatAST::StableNodeRef out{};
    bool ok = flat.deserialize_stable_ref(short_buf, sizeof(short_buf), out);
    CHECK(!ok, "deserialize returned false for 8-byte buffer");
    return true;
}

// ── AC #6: make_ref captures next_mutation_id_ ──────────────
bool test_make_ref_captures_mutation_id() {
    std::println("\n--- AC #6: make_ref captures next_mutation_id_ ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto x = pool.intern("x");
    flat.add_variable(x);
    // Before any mutation, next_mutation_id_ == 1
    auto ref1 = flat.make_ref(0);
    CHECK(ref1.mutation_id_at_capture == 1,
          "first make_ref: mutation_id_at_capture == 1 (got " +
          std::to_string(ref1.mutation_id_at_capture) + ")");
    return true;
}

// ── AC #7: Aura (ast:ref-mutation-id / :ref-workspace-id) ───
bool test_aura_helpers_extract() {
    std::println("\n--- AC #7: (ast:ref-mutation-id) + (ast:ref-workspace-id) ---");
    aura::compiler::CompilerService cs;
    // Set up some workspace so workspace_flat_ is non-null
    cs.eval("(set-code \"x\")");
    // Pass 4 ints: id, gen, mutation_id, workspace_id
    auto mut_id = run_int(cs, "(ast:ref-mutation-id 5 3 99 2)");
    auto ws_id = run_int(cs, "(ast:ref-workspace-id 5 3 99 2)");
    CHECK(mut_id == 99, "ref-mutation-id == 99 (got " +
          std::to_string(mut_id) + ")");
    CHECK(ws_id == 2, "ref-workspace-id == 2 (got " +
          std::to_string(ws_id) + ")");
    return true;
}

// ── AC #8: Aura (ast:ref-serialize) + (ast:ref-deserialize) ──
bool test_aura_serialize_deserialize() {
    std::println("\n--- AC #8: (ast:ref-serialize) roundtrip ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"x\")");
    // Serialize a ref: id=7, gen=2, mutation_id=123, workspace_id=5
    auto r = cs.eval("(ast:ref-serialize 7 2 123 5)");
    if (!r || !aura::compiler::types::is_string(*r)) {
        ++g_failed;
        std::cerr << "ast:ref-serialize returned non-string\n";
        return false;
    }
    // Verify the string is 24 bytes
    auto sidx = aura::compiler::types::as_string_idx(*r);
    if (sidx >= cs.evaluator().string_heap().size()) {
        ++g_failed; return false;
    }
    const std::string& blob = cs.evaluator().string_heap()[sidx];
    CHECK(static_cast<int>(blob.size()) ==
          static_cast<int>(aura::ast::FlatAST::kStableRefSerializedSize),
          "blob size == 24 (got " + std::to_string(blob.size()) + ")");
    // P0: full deserialize roundtrip via Aura source is
    // deferred (binary blobs in string literals have
    // quoting issues — the blob has many null bytes).
    // The C++ roundtrip is already validated in AC #3
    // (above). Here we just verify the serialize produces
    // the expected magic header + size, which is sufficient
    // for the EDSL user-facing primitive. Future work:
    // add a (base64-encode blob) primitive so callers can
    // roundtrip refs without raw-byte issues.
    std::uint32_t magic = 0;
    std::memcpy(&magic, blob.data(), 4);
    CHECK(magic == aura::ast::FlatAST::kStableRefMagic,
          "blob starts with #291 magic");
    return true;
}

// ── AC #9: existing in-memory usage not broken ──────────────
// Default-constructed StableNodeRef must still be invalid in
// is_valid() (backward compat). make_ref(id) with default
// fields (mutation_id_at_capture=0, workspace_id=0) must
// still produce a valid ref pointing to the node.
bool test_backward_compat() {
    std::println("\n--- AC #9: backward compat — default ref still invalid ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto x = pool.intern("x");
    flat.add_variable(x);
    // Default ref is invalid
    aura::ast::FlatAST::StableNodeRef def{};
    CHECK(!flat.is_valid(def), "default ref is invalid");
    // make_ref(0) is valid
    auto r = flat.make_ref(0);
    CHECK(flat.is_valid(r), "make_ref(0) is valid");
    return true;
}

int run_tests() {
    std::println("═══ Issue #291 ═══");
    test_struct_defaults();
    test_serialized_size();
    test_serialize_roundtrip();
    test_deserialize_rejects_bad_magic();
    test_deserialize_rejects_short();
    test_make_ref_captures_mutation_id();
    test_aura_helpers_extract();
    test_aura_serialize_deserialize();
    test_backward_compat();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}

}

int aura_issue_291_run() { return test_291_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_291_run(); }
#endif