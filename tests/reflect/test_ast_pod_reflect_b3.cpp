// Wave B3: small AST public PODs via auto_serialize / to_json.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "reflect/ast_pod_reflect.hh"
#include "reflect/reflect.hh"

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
    using namespace aura::ast_pod;

    // ── SourceLocation ───────────────────────────────────────
    {
        constexpr auto m = aura::reflect::reflect_members<SourceLocation>();
        static_assert(m.size() == 3);
        check(m[0].name == "line" && m[1].name == "column" && m[2].name == "file", "loc names");

        SourceLocation s{.line = 12, .column = 4, .file = 2};
        auto bytes = pod_serialize(s);
        check(bytes.size() == 12, "loc wire 12");
        auto r = pod_deserialize<SourceLocation>(bytes);
        check(r.line == 12 && r.column == 4 && r.file == 2, "loc rt");
        check(validate_source_location(s), "loc validate");
        const std::string j = pod_json(s);
        check(j.find("\"line\":12") != std::string::npos, "loc json");
        check(j.find("\"column\":4") != std::string::npos, "loc json col");
    }

    // ── Patch ────────────────────────────────────────────────
    {
        constexpr auto m = aura::reflect::reflect_members<Patch>();
        static_assert(m.size() == 3);
        check(m[0].name == "node", "patch node name");

        Patch p{.node = 42, .field_offset = 8, .new_value = 0xdeadbeefULL};
        auto bytes = pod_serialize(p);
        check(bytes.size() == 16, "patch wire 16");
        auto r = pod_deserialize<Patch>(bytes);
        check(r.node == 42 && r.field_offset == 8 && r.new_value == 0xdeadbeefULL, "patch rt");
        check(validate_patch(p), "patch validate");
        Patch bad = p;
        bad.field_offset = 2'000'000u;
        check(!validate_patch(bad), "patch reject huge offset");
        const std::string j = pod_json(p);
        check(j.find("\"node\":42") != std::string::npos, "patch json");
    }

    // ── MatchClauseInfo (vector<SymId>) ──────────────────────
    {
        constexpr auto m = aura::reflect::reflect_members<MatchClauseInfo>();
        static_assert(m.size() == 5);
        check(m[0].name == "used_constructors", "mci used name");
        check(m[0].kind == aura::reflect::MemberKind::Vector, "mci used vector");

        MatchClauseInfo info;
        info.used_constructors = {1, 2, 3};
        info.candidate_constructors = {9, 8};
        info.has_wildcard = true;
        info.exhaustiveness_checked = true;
        info.subject_type_id = 7;
        auto bytes = pod_serialize(info);
        auto r = pod_deserialize<MatchClauseInfo>(bytes);
        check(r.used_constructors.size() == 3 && r.used_constructors[2] == 3, "mci used rt");
        check(r.candidate_constructors.size() == 2 && r.candidate_constructors[0] == 9, "mci cand");
        check(r.has_wildcard && r.exhaustiveness_checked && r.subject_type_id == 7, "mci flags");
        check(validate_match_clause(info), "mci validate");
        const std::string j = pod_json(info);
        check(j.find("\"used_constructors\":[1,2,3]") != std::string::npos, "mci json used");
        check(j.find("\"has_wildcard\":true") != std::string::npos, "mci json wild");
    }

    // ── NodeLifecycleStats ───────────────────────────────────
    {
        NodeLifecycleStats s{
            .total_slots = 100, .live_nodes = 80, .free_slots = 20, .fragmentation_ratio = 0.25};
        auto bytes = pod_serialize(s);
        auto r = pod_deserialize<NodeLifecycleStats>(bytes);
        check(r.total_slots == 100 && r.live_nodes == 80 && r.free_slots == 20, "life counts");
        check(r.fragmentation_ratio == 0.25, "life ratio");
        check(pod_json(s).find("\"live_nodes\":80") != std::string::npos, "life json");
    }

    // ── PostRestoreReport ────────────────────────────────────
    {
        constexpr auto m = aura::reflect::reflect_members<PostRestoreReport>();
        static_assert(m.size() == 4);
        check(m[1].name == "generation", "post gen name");
        // u16 generation → Int16/UInt16
        check(m[1].kind == aura::reflect::MemberKind::UInt16 ||
                  m[1].kind == aura::reflect::MemberKind::Int16,
              "post gen int16");

        PostRestoreReport p{.violations = 1, .generation = 3, .live_nodes = 10, .free_slots = 2};
        auto bytes = pod_serialize(p);
        auto r = pod_deserialize<PostRestoreReport>(bytes);
        check(r.violations == 1 && r.generation == 3, "post rt");
        check(r.live_nodes == 10 && r.free_slots == 2, "post counts");
        check(pod_json(p).find("\"generation\":3") != std::string::npos, "post json");
    }

    std::printf("test_ast_pod_reflect_b3: %s (failed=%d)\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed == 0 ? 0 : 1;
}
