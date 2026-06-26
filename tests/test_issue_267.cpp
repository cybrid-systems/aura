// @category: integration
// @reason: uses CompilerService; tests #267 macro-introduced query opt-in


import std;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_267_detail {
static int g_passed = 0;
static int g_failed = 0;

static aura::compiler::types::EvalValue run(aura::compiler::CompilerService& cs,
                                            std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return aura::compiler::types::make_void();
    }
    return *r;
}

static std::int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto v = run(cs, src);
    if (!aura::compiler::types::is_int(v))
        return -1;
    return aura::compiler::types::as_int(v);
}

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                          \
            std::println("  PASS: {}", msg);                                                       \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println("  FAIL: {}", msg);                                                       \
        }                                                                                          \
    } while (0)

static std::int64_t pattern_count(aura::compiler::CompilerService& cs, const std::string& code,
                                  const std::string& pattern, bool include_macro,
                                  bool expand_macros = true) {
    std::string script = std::string("(begin (set-code \"") + code + "\")";
    if (expand_macros)
        script += " (eval-current)";
    script += " (length (query:pattern \"" + pattern + "\"";
    if (include_macro)
        script += " :include-macro-introduced #t";
    script += ")))";
    return run_int(cs, script);
}

static std::int64_t query_after_setup(aura::compiler::CompilerService& cs,
                                      const std::string& code, std::string_view query_expr) {
    return run_int(cs, std::string("(begin (set-code \"") + code + "\") (eval-current) " +
                                  std::string(query_expr) + ")");
}

bool test_pattern_default_skips_macro() {
    std::println("\n--- AC1: query:pattern skips macro-introduced by default ---");
    aura::compiler::CompilerService cs;
    // Pattern (+ 7 7) only appears after hygienic expansion, not in macro body (+ x x).
    std::string code = "(define-hygienic-macro (twice x) (+ x x)) (twice 7)";
    auto n = pattern_count(cs, code, "(+ 7 7)", false);
    CHECK(n == 0, "default hygiene skips macro-expanded (+ 7 7)");
    return true;
}

bool test_pattern_include_macro_flag() {
    std::println("\n--- AC2: :include-macro-introduced #t matches macro nodes ---");
    aura::compiler::CompilerService cs;
    std::string code = "(define-hygienic-macro (twice x) (+ x x)) (twice 7)";
    auto without = pattern_count(cs, code, "(+ 7 7)", false);
    auto with_flag = pattern_count(cs, code, "(+ 7 7)", true);
    CHECK(without == 0, "without flag: 0 matches");
    CHECK(with_flag >= 1, "with flag: at least one macro-introduced match");
    return true;
}

bool test_syntax_marker_where_alias() {
    std::println("\n--- AC3: :syntax-marker alias in query:where ---");
    aura::compiler::CompilerService cs;
    auto marker_count = query_after_setup(
        cs, "(define-hygienic-macro (twice x) (+ x x)) (twice 3)",
        "(length (query:filter (query:where :syntax-marker \"MacroIntroduced\")))");
    auto macro_count = query_after_setup(
        cs, "(define-hygienic-macro (twice x) (+ x x)) (twice 3)",
        "(length (query:macro-introduced))");
    CHECK(marker_count >= 0, ":syntax-marker filter returns non-negative count");
    CHECK(marker_count == macro_count, ":syntax-marker matches query:macro-introduced count");
    return true;
}

bool test_filter_marker_and_node_type() {
    std::println("\n--- AC4: query:filter composes :syntax-marker + :node-type ---");
    aura::compiler::CompilerService cs;
    auto calls = query_after_setup(
        cs, "(define-hygienic-macro (twice x) (+ x x)) (twice 3)",
        "(length (query:filter "
        "(query:where :syntax-marker \"MacroIntroduced\") "
        "(query:where :node-type \"Call\")))");
    CHECK(calls >= 1, "finds macro-introduced Call nodes");
    return true;
}

bool test_include_macro_false_explicit() {
    std::println("\n--- AC5: :include-macro-introduced #f matches default ---");
    aura::compiler::CompilerService cs;
    std::string code = "(define-hygienic-macro (twice x) (+ x x)) (twice 7) (+ 7 7)";
    auto def = pattern_count(cs, code, "(+ 7 7)", false);
    auto explicit_f = run_int(cs, std::string("(begin (set-code \"") + code +
                                              "\") (eval-current) (length (query:pattern "
                                              "\"(+ 7 7)\" :include-macro-introduced #f)))");
    CHECK(def == explicit_f, "explicit #f equals default skip behavior");
    CHECK(def >= 1, "user-written (+ 7 7) still matches when macro expansion is skipped");
    return true;
}

bool test_user_code_still_matches_with_flag() {
    std::println("\n--- AC6: user-written code still matches with flag ---");
    aura::compiler::CompilerService cs;
    auto n = pattern_count(cs, "(+ 1 2)", "(+ 1 2)", true, false);
    CHECK(n >= 1, "user (+ 1 2) matches with include flag");
    return true;
}

int run_tests() {
    std::println("═══ Issue #267 — macro-introduced query opt-in ═══");
    test_pattern_default_skips_macro();
    test_pattern_include_macro_flag();
    test_syntax_marker_where_alias();
    test_filter_marker_and_node_type();
    test_include_macro_false_explicit();
    test_user_code_still_matches_with_flag();
    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_267_detail

int aura_issue_267_run() { return aura_issue_267_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_267_run(); }
#endif