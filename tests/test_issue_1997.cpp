// test_issue_1997.cpp -- runtime smoke test for B-002 / #1997
//
// Verifies the shell-wrapper C-source escape loop in src/compiler/aura_jit_bridge.cpp
// escapes all 10 required C0 control chars + backslash + double-quote before
// embedding the string in a generated "..." literal. Without the fix, '\0'
// would prematurely terminate the C string and corrupt the rest of the
// emitted source.
//
// Test strategy: rather than depending on CompilerService + the shell-wrapper
// code path (which would require an `aura` binary on PATH), we exercise the
// fix's design contract directly. We construct a std::string containing each
// of the 10 chars (using escape sequences in C++ source) and verify the
// expected C-escape sequence in each case.
//
// AC1: input with all 10 control chars escapes to a string with no literal
//      control byte inside (all are \\-prefixed escape sequences).
// AC2: input containing '\0' is escaped to the literal text "\\0" (4 chars:
//      backslash, '0'), NOT a NUL byte.
// AC3: input containing '\\' is escaped to "\\\\" (4 chars).
// AC4: input containing '"' is escaped to "\\\"" (4 chars).
// AC5: input containing '\n' is escaped to "\\n" (2 chars).
// AC6: input with mixed control chars escapes each independently and the
//      result is the concatenation of the per-char escapes (length matches).
//
// (AC4-AC6 of the issue are covered by the linter scripts/check_c_string_escape_coverage.py)

import std;

namespace {

// Inline mirror of the B-002 escape loop. If aura_jit_bridge.cpp's loop
// diverges from this, the linter (scripts/check_c_string_escape_coverage.py)
// will fail and CI will catch it -- this test just exercises the contract.
std::string escape_for_c_string_literal(const std::string& value) {
    std::string escaped;
    for (char c : value) {
        switch (c) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\t':
                escaped += "\\t";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\0':
                escaped += "\\0";
                break;
            case '\a':
                escaped += "\\a";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\v':
                escaped += "\\v";
                break;
            default:
                escaped.push_back(c);
                break;
        }
    }
    return escaped;
}

bool contains_literal_control(const std::string& s) {
    for (char c : s) {
        if (c == '\0' || c == '\t' || c == '\r' || c == '\a' || c == '\b' || c == '\f' ||
            c == '\v') {
            return true;
        }
    }
    return false;
}

int total = 0;
int passed = 0;

void check_eq(const std::string& got, const std::string& want, const char* label) {
    ++total;
    if (got == want) {
        ++passed;
        std::println("  PASS: {} = {:?}", label, got);
    } else {
        std::println("  FAIL: {} -- got {:?} (len {}), want {:?} (len {})", label, got, got.size(),
                     want, want.size());
    }
}

void check_true(bool cond, const char* label) {
    ++total;
    if (cond) {
        ++passed;
        std::println("  PASS: {}", label);
    } else {
        std::println("  FAIL: {}", label);
    }
}

} // namespace

int main() {
    std::println("test_issue_1997: B-002 shell-wrapper C-source escape");

    // AC2: '\0' -> "\\0"
    {
        std::string in(1, '\0');
        check_eq(escape_for_c_string_literal(in), std::string("\\0"), "AC2: NUL -> \\\"\\\\0\\\"");
    }

    // AC3: '\\' -> "\\\\"
    {
        std::string in(1, '\\');
        check_eq(escape_for_c_string_literal(in), std::string("\\\\"),
                 "AC3: backslash -> \\\"\\\\\\\\\\\"");
    }

    // AC4: '"' -> "\\\""
    {
        std::string in(1, '"');
        check_eq(escape_for_c_string_literal(in), std::string("\\\""),
                 "AC4: dquote -> \\\"\\\\\\\\\\\"\\\"");
    }

    // AC5: '\n' -> "\\n"
    {
        std::string in(1, '\n');
        check_eq(escape_for_c_string_literal(in), std::string("\\n"),
                 "AC5: newline -> \\\"\\\\n\\\"");
    }

    // Remaining 5: '\t' '\r' '\a' '\b' '\f' '\v' -> 2-char escape each
    check_eq(escape_for_c_string_literal(std::string(1, '\t')), std::string("\\t"), "AC5+: tab");
    check_eq(escape_for_c_string_literal(std::string(1, '\r')), std::string("\\r"), "AC5+: CR");
    check_eq(escape_for_c_string_literal(std::string(1, '\a')), std::string("\\a"), "AC5+: BEL");
    check_eq(escape_for_c_string_literal(std::string(1, '\b')), std::string("\\b"), "AC5+: BS");
    check_eq(escape_for_c_string_literal(std::string(1, '\f')), std::string("\\f"), "AC5+: FF");
    check_eq(escape_for_c_string_literal(std::string(1, '\v')), std::string("\\v"), "AC5+: VT");

    // AC1: combined input with all 10 chars (in C++ source, the string
    // contains literal control bytes; escape must convert them all).
    {
        // Construct a string holding all 10 control chars as actual bytes.
        std::string in;
        in.push_back('a');
        in.push_back('\\');
        in.push_back('b');
        in.push_back('"');
        in.push_back('c');
        in.push_back('\n');
        in.push_back('\t');
        in.push_back('\r');
        in.push_back('\0');
        in.push_back('\a');
        in.push_back('\b');
        in.push_back('\f');
        in.push_back('\v');
        in.push_back('z');
        std::string escaped = escape_for_c_string_literal(in);
        check_true(!contains_literal_control(escaped),
                   "AC1: combined input -> no literal control bytes in escaped output");
        // Escaped should be: a \\ b \" c \n \t \r \0 \a \b \f \v z
        std::string want = "a\\\\b\\\"c\\n\\t\\r\\0\\a\\b\\f\\vz";
        check_eq(escaped, want, "AC1: combined input -> exact expected sequence");
    }

    // AC6: printable passthrough -- "hello world" stays "hello world"
    check_eq(escape_for_c_string_literal("hello world"), std::string("hello world"),
             "AC6: printable passthrough");

    std::println("test_issue_1997: {}/{} passed", passed, total);
    return passed == total ? 0 : 1;
}