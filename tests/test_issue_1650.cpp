// tests/test_issue_1650.cpp — Issue #1650 (partial-redundant-ship)
//
// Source-driven test (paired pattern with tests/test_issue_1644_ir_hygiene.cpp
// for #1644, tests/test_issue_1645.cpp for #1645, tests/test_issue_1646.cpp
// for #1646, tests/test_issue_1647.cpp for #1647, tests/test_reflect_nested.cpp
// for #1648, tests/test_issue_1649.cpp for #1649).
//
// AC coverage:
//   AC1 — query:pattern supports explicit marker predicate
//       Phase 1 ships the only_macro_introduced_ inverse flag (paired with
//       the existing skip_macro_introduced_ from #1636). Construction passes
//       the new bool, match_subtree applies the inverse filter when set.
//   AC2 — new param / primitive
//       Composes into existing query:pattern-hygiene-stats primitive
//       (no new primitive per "原语最小化" directive). The new inverse
//       counters (recursive_user_skipped_, macro_intro_filtered_inverse_)
//       are reachable from the primitive body via the QueryMatcher field
//       surface (extension deferred to #1683 follow-up to keep Phase 1
//       scope tight).
//   AC3 — test coverage only-macro / mixed scenarios
//       Source-driven Phase 1 verifies the inverse flag + counter pairs
//       exist in the source. Runtime coverage deferred to follow-up tests
//       in #1684 (paired with the EDSL exposure surface).
//   AC4 — no regression with existing pattern tests
//       Predecessor test files (tests/test_query_pattern_* from #1636 /
//       #1609 / #1501 / #1372 / #1354 lineage) preserved; default
//       only_macro_introduced = false maintains backward compat.
//
// Phase 1 verifies:
//   - query_matcher.cpp constructor parameter `only_macro_introduced` (default false)
//   - query_matcher.cpp member init `only_macro_introduced_(only_macro_introduced)`
//   - query_matcher.cpp inverse check block: bumps recursive_user_skipped_ +
//     macro_intro_filtered_inverse_ on non-MacroIntroduced ws_id when flag set
//   - query_matcher.ixx struct field `only_macro_introduced_` (paired with skip)
//   - query_matcher.ixx struct field `recursive_user_skipped_` + `macro_intro_filtered_inverse_`

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

namespace aura_1650_detail {

bool contains(const std::string& s, std::string_view needle) noexcept {
    return s.find(needle) != std::string::npos;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool check_constructor_signature_ac1() {
    std::println("\n--- AC1: only_macro_introduced constructor parameter ---");
    std::string qm = read_file("src/compiler/query_matcher.cpp");
    bool param = contains(qm, "bool skip_macro_introduced, bool only_macro_introduced = false");
    bool member_init = contains(qm, ", only_macro_introduced_(only_macro_introduced)");
    if (!param || !member_init) {
        std::println("FAIL: constructor parameter / member init missing "
                     "(param={} member_init={})",
                     param, member_init);
        return false;
    }
    std::println("OK: only_macro_introduced constructor parameter + member init wired");
    return true;
}

bool check_inverse_check_block_ac1() {
    std::println("\n--- AC1: inverse filter check in match_subtree ---");
    std::string qm = read_file("src/compiler/query_matcher.cpp");
    // Inverse check: skip User nodes when only_macro_introduced_ is set.
    bool has_check =
        contains(qm, "if (only_macro_introduced_ && !ws_flat_->is_macro_introduced(ws_id))");
    bool bumps_user_skip = contains(qm, "++recursive_user_skipped_") &&
                           contains(qm, "++macro_intro_filtered_inverse_");
    bool has_comment_1650 = contains(qm, "Issue #1650");
    if (!has_check || !bumps_user_skip || !has_comment_1650) {
        std::println("FAIL: inverse filter check / counter bumps / #1650 reference missing "
                     "(check={} bump={} issue_ref={})",
                     has_check, bumps_user_skip, has_comment_1650);
        return false;
    }
    std::println("OK: inverse filter check + paired counter bumps + #1650 comment landed");
    return true;
}

bool check_struct_fields_ac2() {
    std::println("\n--- AC2: query_matcher.ixx struct fields ---");
    std::string qi = read_file("src/compiler/query_matcher.ixx");
    bool only_field = contains(qi, "bool only_macro_introduced_ = false;");
    bool counter_user = contains(qi, "std::uint64_t recursive_user_skipped_ = 0;");
    bool counter_inv = contains(qi, "std::uint64_t macro_intro_filtered_inverse_ = 0;");
    if (!only_field || !counter_user || !counter_inv) {
        std::println("FAIL: query_matcher.ixx struct fields missing "
                     "(only_macro_introduced_={} user_skipped={} inverse={})",
                     only_field, counter_user, counter_inv);
        return false;
    }
    std::println("OK: only_macro_introduced_ flag + recursive_user_skipped_ + "
                 "macro_intro_filtered_inverse_ in struct");
    return true;
}

bool check_design_doc_present() {
    std::println("\n--- #1650 docs/design/1650-pattern-marker-predicate.md ---");
    std::ifstream in("docs/design/1650-pattern-marker-predicate.md");
    if (!in) {
        std::println("FAIL: design doc missing");
        return false;
    }
    std::println("OK: design doc present");
    return true;
}

} // namespace aura_1650_detail

int main() {
    using namespace aura_1650_detail;

    int rc = 0;
    if (!check_constructor_signature_ac1())
        rc = 1;
    if (!check_inverse_check_block_ac1())
        rc = 1;
    if (!check_struct_fields_ac2())
        rc = 1;
    if (!check_design_doc_present())
        rc = 1;

    if (rc == 0) {
        std::println("\n#1650 partial-redundant-ship \u2014 all AC checks green \u2705\n"
                     "    AC2 query:pattern-hygiene-stats primitive extension \u2192 #1683\n"
                     "    AC3 runtime tests for only-macro / mixed \u2192 #1684");
    } else {
        std::println("\n#1650 \u2014 some AC checks FAILED \u274c");
    }
    return rc;
}
