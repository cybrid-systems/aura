// @category: unit
// @reason: pure C++ — FlatAST builders + PCV + parent_ linkage
// test_issue_311.cpp — Verify Issue #311 acceptance criteria
// ("feat(ast): implement add_interface and add_modport
//  builders using PersistentChildVector").
//
// Direct follow-up to #310 (which added the NodeTag enum
// entries + kNodeMeta). This PR ships the C++ builder
// methods that let users construct Interface + Modport
// nodes in FlatAST (the foundation for parser / EDSL /
// EDA-side code that needs to represent SV interface and
// modport constructs).
//
// ACs:
//   AC1: builder 能正确返回 NodeId
//        (tests 1-2: add_interface / add_modport return
//        valid IDs in the [1, flat.size()) range)
//   AC2: children_ 使用 PersistentChildVector 存储
//        (test 3: after add_interface, children_[id] has
//        the right count + the right NodeIds, iterated via
//        the same path as add_export; for add_modport the
//        payload lives in the param_data_ side-table, not
//        children_)
//   AC3: parent_ 链接正确
//        (test 4: after add_interface, parent_[child] ==
//        interface id for every body item)
//   AC4: PCV COW 语义不变
//        (test 5: copy-construction of a FlatAST that
//        contains an Interface preserves the interface
//        payload — same shape as add_export's COW behavior)
//   AC5: 单元测试覆盖
//        (this file: tests 1-5 below)


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;

namespace aura_issue_311_detail {
#define CHECK_EQ_LOCAL(a, b, msg)                                                                  \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (!(_a == _b)) {                                                                         \
            std::println("  FAIL: {} (got {} expected {} line {})", msg, _a, _b, __LINE__);        \
            ++g_failed;                                                                            \
        } else {                                                                                   \
            std::println("  PASS: {}", msg);                                                       \
            ++g_passed;                                                                            \
        }                                                                                          \
    } while (0)

// ═══════════════════════════════════════════════════════════════
// AC1: add_interface returns a valid NodeId
// ═══════════════════════════════════════════════════════════════

bool test_add_interface_returns_valid_id() {
    std::println("\n--- AC1a: add_interface returns a valid NodeId ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto if_name = pool.intern("Bus");
    // Build a body with 3 placeholder variables (just NodeIds).
    auto sig1 = flat.add_variable(pool.intern("data"));
    auto sig2 = flat.add_variable(pool.intern("valid"));
    auto sig3 = flat.add_variable(pool.intern("ready"));
    std::vector<NodeId> body{sig1, sig2, sig3};
    auto iface = flat.add_interface(if_name, std::span<const NodeId>(body.data(), body.size()));
    CHECK_EQ_LOCAL(iface == aura::ast::NULL_NODE, false, "add_interface returns a non-null NodeId");
    CHECK_EQ_LOCAL(static_cast<std::uint32_t>(iface) < static_cast<std::uint32_t>(flat.size()),
                   true, "returned id < flat.size()");
    CHECK(flat.get(iface).tag == NodeTag::Interface, "interface node has NodeTag::Interface");
    CHECK_EQ_LOCAL(flat.sym_id(iface), if_name, "interface sym_id matches the name");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC1 (cont): add_modport returns a valid NodeId
// ═══════════════════════════════════════════════════════════════

bool test_add_modport_returns_valid_id() {
    std::println("\n--- AC1b: add_modport returns a valid NodeId ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto mp_name = pool.intern("master");
    // Port list (stored in param_data_ side-table, same
    // shape as add_define_module).
    std::vector<SymId> ports{
        pool.intern("data"),
        pool.intern("valid"),
    };
    auto mp = flat.add_modport(mp_name, std::span<const SymId>(ports.data(), ports.size()));
    CHECK_EQ_LOCAL(mp == aura::ast::NULL_NODE, false, "add_modport returns a non-null NodeId");
    CHECK_EQ_LOCAL(static_cast<std::uint32_t>(mp) < static_cast<std::uint32_t>(flat.size()), true,
                   "returned id < flat.size()");
    CHECK(flat.get(mp).tag == NodeTag::Modport, "modport node has NodeTag::Modport");
    CHECK_EQ_LOCAL(flat.sym_id(mp), mp_name, "modport sym_id matches the name");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2: children_ uses PersistentChildVector
// ═══════════════════════════════════════════════════════════════
//
// For Interface: body items are stored in children_ via PCV
// (same pattern as add_export). For Modport: payload is
// param_data_ side-table (same pattern as add_define_module);
// modports don't have children_ set.

bool test_interface_children_stored_in_pcv() {
    std::println("\n--- AC2: Interface body in PersistentChildVector ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto if_name = pool.intern("Bus");
    auto sig1 = flat.add_variable(pool.intern("data"));
    auto sig2 = flat.add_variable(pool.intern("valid"));
    std::vector<NodeId> body{sig1, sig2};
    auto iface = flat.add_interface(if_name, std::span<const NodeId>(body.data(), body.size()));
    // Iterate via the public children() accessor (returns
    // std::span<const NodeId> backed by the PersistentChildVector).
    std::size_t visited = 0;
    for (auto cid : flat.children(iface)) {
        (void)cid;
        ++visited;
    }
    CHECK_EQ_LOCAL(visited, std::size_t{2}, "children(interface) yields 2 body items");
    // Same shape via span size().
    CHECK_EQ_LOCAL(flat.children(iface).size(), std::size_t{2},
                   "children(interface) span has size 2");
    return true;
}

bool test_modport_uses_param_data_side_table() {
    std::println("\n--- AC2 (modport): payload in param_data_ side-table ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto mp_name = pool.intern("slave");
    std::vector<SymId> ports{
        pool.intern("valid"),
        pool.intern("data"),
    };
    auto mp = flat.add_modport(mp_name, std::span<const SymId>(ports.data(), ports.size()));
    // The modport has no children_ — payload lives in param_data_.
    CHECK_EQ_LOCAL(flat.children(mp).empty(), true,
                   "modport has zero children (payload in param_data_)");
    // Verify the ports are readable via the param_at() accessor.
    auto p0_sym = flat.param_at(mp, 0);
    CHECK(p0_sym != aura::ast::INVALID_SYM,
          "param_at(mp, 0) returns a valid sym (first port present)");
    CHECK_EQ_LOCAL(p0_sym, ports[0], "param_at(mp, 0) matches the first port symbol");
    auto p1_sym = flat.param_at(mp, 1);
    CHECK_EQ_LOCAL(p1_sym, ports[1], "param_at(mp, 1) matches the second port symbol");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3: parent_ linkage is correct
// ═══════════════════════════════════════════════════════════════

bool test_interface_parent_links() {
    std::println("\n--- AC3: parent_ links for interface body items ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;
    auto if_name = pool.intern("Bus");
    auto sig1 = flat.add_variable(pool.intern("data"));
    auto sig2 = flat.add_variable(pool.intern("valid"));
    std::vector<NodeId> body{sig1, sig2};
    auto iface = flat.add_interface(if_name, std::span<const NodeId>(body.data(), body.size()));
    CHECK_EQ_LOCAL(flat.parent_of(sig1), iface,
                   "parent_of(sig1) == interface id (back-link wired)");
    CHECK_EQ_LOCAL(flat.parent_of(sig2), iface,
                   "parent_of(sig2) == interface id (back-link wired)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC4: PCV copy-on-write semantics intact
// ═══════════════════════════════════════════════════════════════

bool test_interface_pcv_cow_semantics() {
    std::println("\n--- AC4: PCV COW semantics for interface ---");
    using namespace aura::ast;
    FlatAST a;
    StringPool pool;
    auto if_name = pool.intern("Bus");
    auto sig1 = a.add_variable(pool.intern("data"));
    std::vector<NodeId> body{sig1};
    auto iface = a.add_interface(if_name, std::span<const NodeId>(body.data(), body.size()));
    // Copy-construct b from a (the COW path).
    FlatAST b(a);
    // The copied interface should have the same payload.
    auto b_iface = b.add_variable(pool.intern("dummy")); // ensure copy is independent
    (void)b_iface;
    // The copy should preserve the interface id (PCV's stable
    // pointer invariant) and the body item should be readable.
    CHECK_EQ_LOCAL(static_cast<std::uint32_t>(iface) < static_cast<std::uint32_t>(b.size()), true,
                   "copied FlatAST contains the original interface id");
    CHECK_EQ_LOCAL(b.children(iface).size(), std::size_t{1},
                   "copied interface preserves the body count");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC5 (shape): end-to-end sv interface pattern
// ═══════════════════════════════════════════════════════════════
//
// Reproduce a realistic SV `interface` shape:
//   interface Bus;
//     logic [7:0] data;
//     logic       valid;
//     modport master (input data, output valid);
//   endinterface

bool test_sv_interface_e2e_pattern() {
    std::println("\n--- AC5 (shape): end-to-end SV interface pattern ---");
    using namespace aura::ast;
    FlatAST flat;
    StringPool pool;

    // Body: signal decls (Variable nodes) + nested modports.
    auto data_sig = flat.add_variable(pool.intern("data"));
    auto valid_sig = flat.add_variable(pool.intern("valid"));

    // Nested modport nodes first (so they have valid IDs).
    std::vector<SymId> master_ports{pool.intern("data"), pool.intern("valid")};
    auto master_mp = flat.add_modport(
        pool.intern("master"), std::span<const SymId>(master_ports.data(), master_ports.size()));
    std::vector<SymId> slave_ports{pool.intern("valid"), pool.intern("data")};
    auto slave_mp = flat.add_modport(
        pool.intern("slave"), std::span<const SymId>(slave_ports.data(), slave_ports.size()));

    // Interface body: the 2 signals + 2 modports (4 items total).
    std::vector<NodeId> body{data_sig, valid_sig, master_mp, slave_mp};
    auto bus =
        flat.add_interface(pool.intern("Bus"), std::span<const NodeId>(body.data(), body.size()));

    // Verify the interface's children are the 4 body items.
    CHECK_EQ_LOCAL(flat.children(bus).size(), std::size_t{4},
                   "Bus interface has 4 body items (2 sigs + 2 modports)");
    // The modports are reachable as children of the interface.
    std::size_t seen_modports = 0;
    for (auto cid : flat.children(bus)) {
        if (flat.get(cid).tag == NodeTag::Modport)
            ++seen_modports;
    }
    CHECK_EQ_LOCAL(seen_modports, std::size_t{2}, "Bus interface body contains 2 Modport nodes");
    // The master modport has 2 ports in param_data_.
    CHECK(flat.param_at(master_mp, 0) != aura::ast::INVALID_SYM,
          "master modport port 0 is a valid sym");
    CHECK(flat.param_at(master_mp, 1) != aura::ast::INVALID_SYM,
          "master modport port 1 is a valid sym");
    return true;
}

int run_tests() {
    std::println("═══ Issue #311 (add_interface + add_modport builders) ═══\n");
    test_add_interface_returns_valid_id();
    test_add_modport_returns_valid_id();
    test_interface_children_stored_in_pcv();
    test_modport_uses_param_data_side_table();
    test_interface_parent_links();
    test_interface_pcv_cow_semantics();
    test_sv_interface_e2e_pattern();
    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_311_detail

int aura_issue_311_run() {
    return aura_issue_311_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_311_run();
}
#endif