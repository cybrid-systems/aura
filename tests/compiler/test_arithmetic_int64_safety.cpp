// test_arithmetic_int64_safety.cpp — Issues #1150–#1156 Phase 1

#include "test_harness.hpp"

#include "compiler/float_repr.hh"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;

namespace {

struct CliRun {
    int rc = -1;
    std::string out;
    std::string err;
};

std::string aura_binary() {
    if (const char* e = std::getenv("AURA_BIN")) {
        if (::access(e, X_OK) == 0)
            return e;
    }
    char buf[4096];
    const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string dir(buf);
        const auto slash = dir.rfind('/');
        if (slash != std::string::npos) {
            auto cand = dir.substr(0, slash + 1) + "aura";
            if (::access(cand.c_str(), X_OK) == 0)
                return cand;
        }
    }
    for (const char* c : {"./aura", "./build/aura", "../aura", "../build/aura"}) {
        if (::access(c, X_OK) == 0)
            return c;
    }
    return {};
}

CliRun run_aura_file(const std::string& bin, const std::string& src, const char* face,
                     const char* outp = "/tmp/aura-4461-out.txt",
                     const char* errp = "/tmp/aura-4461-err.txt") {
    CliRun r;
    std::string cmd = "timeout 15 env -u AURA_SANDBOX -u AURA_PIPELINE_STRICT ";
    if (face && face[0] != '\0')
        cmd += face;
    cmd += " '";
    cmd += bin;
    cmd += "' '";
    cmd += src;
    cmd += "' >'";
    cmd += outp;
    cmd += "' 2>'";
    cmd += errp;
    cmd += "'";
    const int raw = std::system(cmd.c_str());
    r.rc = (raw != -1 && WIFEXITED(raw)) ? WEXITSTATUS(raw) : raw;
    {
        std::ifstream in(outp);
        r.out.assign(std::istreambuf_iterator<char>(in), {});
    }
    {
        std::ifstream in(errp);
        r.err.assign(std::istreambuf_iterator<char>(in), {});
    }
    return r;
}

bool write_text(const char* path, const std::string& body) {
    std::ofstream out(path);
    if (!out)
        return false;
    out << body;
    return true;
}

} // namespace

int main() {
    CompilerService cs;

    {
        auto r = cs.eval("(+ 1 2 3)");
        CHECK(r && is_int(*r) && as_int(*r) == 6, "small +");
        auto r2 = cs.eval("(* 2 3 4)");
        CHECK(r2 && is_int(*r2) && as_int(*r2) == 24, "small *");
        auto r3 = cs.eval("(- 10 3)");
        CHECK(r3 && is_int(*r3) && as_int(*r3) == 7, "small -");
        auto r4 = cs.eval("(/ 100 5)");
        CHECK(r4 && is_int(*r4) && as_int(*r4) == 20, "small /");
    }

    // #1151: (/ 0) must not UB — eval fails or returns error/void safely
    {
        auto r = cs.eval("(/ 0)");
        // DivisionByZero surfaces as unexpected Diagnostic → empty optional
        CHECK(!r || is_error(*r) || is_void(*r), "(/ 0) is safe (error/void/nullopt)");
    }

    // #1150/#1152: large mul should not crash — may float-promote
    {
        auto r = cs.eval("(* 1000000 1000000 1000000 1000000)");
        CHECK(r, "large * returns");
        CHECK(is_int(*r) || is_float(*r) || is_error(*r), "large * is int, float or overflow error (#4482)");
    }

    // Issue #4462 / #4482: the fixnum domain is the symmetric
    // [-2^62, 2^62-1]. Literals and string->number outside it read as
    // floats. Integer arithmetic that leaves it is an error value, not a
    // silent inexact float.
    {
        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max() >> 1;
        constexpr std::int64_t kTwo62 = kMax + 1;
        constexpr std::int64_t kNegTwo62 = std::numeric_limits<std::int64_t>::min() >> 1;
        auto exact_int = [&](const char* src, std::int64_t want, const char* msg) {
            auto r = cs.eval(src);
            CHECK(r && is_int(*r) && as_int(*r) == want, msg);
        };
        auto exact_float = [&](const char* src, double want, const char* msg) {
            auto r = cs.eval(src);
            CHECK(r && is_float(*r) && as_float(*r) == want, msg);
        };
        auto overflow = [&](const char* src, const char* msg) {
            auto r = cs.eval(src);
            CHECK(!r || is_error(*r), msg);
        };
        exact_int("4611686018427387903", kMax, "2^62-1 stays int");
        exact_int("-4611686018427387904", kNegTwo62, "-2^62 is int (#4482)");
        exact_int("-4999999999999999", -4999999999999999LL, "-5e15+1 stays int");
        exact_int("-5000000000000000", -5000000000000000LL, "-5e15 is int (#4482)");
        exact_int("-2305843009213693952", -2305843009213693952LL, "-2^61 is int");
        exact_int("(+ 2305843009213693951 2305843009213693951)", 4611686018427387902LL,
                  "(+ c c) stays int");
        exact_int("(+ -2305843009213693952 -2305843009213693952)", kNegTwo62, "(+ d d) is -2^62");
        exact_int("(- 4611686018427387903)", -kMax, "negation of 2^62-1 is int");
        exact_int("(- 0 4611686018427387903)", -kMax, "(- 0 2^62-1) is int");
        exact_int("(* -1 4611686018427387903)", -kMax, "(* -1 2^62-1) is int");
        exact_int("(* 1073741824 2147483648)", 2305843009213693952LL, "2^30*2^31 stays int");
        exact_int("(string->number \"42\")", 42, "string->number 42 stays int");
        exact_int("(string->number \"9007199254740993\")", 9007199254740993LL,
                  "string->number keeps 2^53+1");
        exact_int("(string->number \"+7\")", 7, "string->number keeps a leading plus");
        auto bad = cs.eval("(string->number \"nope\")");
        CHECK(bad && is_bool(*bad) && !aura::compiler::types::as_bool(*bad),
              "string->number failure is #f");
        exact_float("4611686018427387904", static_cast<double>(kTwo62), "literal 2^62 is float");
        exact_float("-4611686018427387905", -4611686018427387905.0, "literal -2^62-1 is float");
        overflow("(+ 2305843009213693951 2305843009213693951 2)", "(+ c c 2) overflows");
        overflow("(* 2 (+ 2305843009213693951 1))", "(* 2 (+ c 1)) overflows");
        overflow("(- -4611686018427387904 1)", "(- -2^62 1) overflows");
        overflow("(- -4611686018427387904)", "(- -2^62) overflows");
        overflow("(* 4294967296 4294967296)", "2^32*2^32 overflows");
        overflow("(* 4611686018427387903 2)", "(2^62-1)*2 overflows");
        exact_float("(string->number \"4611686018427387904\")", static_cast<double>(kTwo62),
                    "string->number 2^62 is float");
        exact_float("(string->number \"9223372036854775808\")", 9223372036854775808.0,
                    "string->number past int64 is float");
        exact_float("9223372036854775808", 9223372036854775808.0, "literal 2^63 is float");
        const double nine = static_cast<double>(9000000000000000000LL);
        exact_float("(+ 9000000000000000000 9000000000000000000)", nine + nine,
                    "float literals past 2^62 add as floats");
        auto quoted = cs.eval("'4294967296");
        CHECK(quoted && is_int(*quoted) && as_int(*quoted) == 4294967296LL,
              "quoted 2^32 stays the full integer");
        auto quoted_big = cs.eval("'4611686018427387904");
        CHECK(quoted_big && is_float(*quoted_big) &&
                  as_float(*quoted_big) == static_cast<double>(kTwo62),
              "quoted 2^62 is float");
    }

    {
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
    }

    // Issue #4461: float zero is IEEE on the literal path; integer zero is
    // an error value, not a normal 0. (/ -0.001 0.0015) stays a real quotient.
    {
        auto p = cs.eval("(/ 1.0 0.0)");
        CHECK(p && is_float(*p) && std::isinf(as_float(*p)) && as_float(*p) > 0,
              "(/ 1.0 0.0) is +inf");
        auto n = cs.eval("(/ -1.0 0.0)");
        CHECK(n && is_float(*n) && std::isinf(as_float(*n)) && as_float(*n) < 0,
              "(/ -1.0 0.0) is -inf");
        auto q = cs.eval("(/ 0.0 0.0)");
        CHECK(q && is_float(*q) && std::isnan(as_float(*q)), "(/ 0.0 0.0) is nan");
        auto rec = cs.eval("(/ 0.0)");
        CHECK(rec && is_float(*rec) && std::isinf(as_float(*rec)) && as_float(*rec) > 0,
              "(/ 0.0) is +inf");
        auto iz = cs.eval("(/ 1 0)");
        CHECK(iz && is_error(*iz) && !is_int(*iz), "(/ 1 0) is an error, not 0");
        auto sm = cs.eval("(/ -0.001 0.0015)");
        CHECK(sm && is_float(*sm) && as_float(*sm) < -0.6 && as_float(*sm) > -0.7,
              "(/ -0.001 0.0015) is a real quotient");
        auto num = cs.eval("(number? (/ 1.0 0.0))");
        CHECK(num && is_bool(*num) && aura::compiler::types::as_bool(*num), "number? of inf");
        auto eq0 = cs.eval("(= (/ 1.0 0.0) 0)");
        CHECK(eq0 && is_bool(*eq0) && !aura::compiler::types::as_bool(*eq0), "inf is not 0");
        auto caught = cs.eval("(try (/ 1 0) (catch (e) e))");
        CHECK(caught && is_string(*caught), "try catches integer division by zero");
    }

    {
        const auto bin = aura_binary();
        CHECK(!bin.empty(), "4461: aura binary");
        if (!bin.empty()) {
            const char* soft = "/tmp/aura-4461-soft.aura";
            CHECK(write_text(soft, "(define (r a b) (/ a b))\n"
                                   "(display (r 1.0 0.0)) (newline)\n"
                                   "(display (r -1.0 0.0)) (newline)\n"
                                   "(display (r 0.0 0.0)) (newline)\n"
                                   "(display (try (r 1 0) (catch (e) e))) (newline)\n"
                                   "(let ((a 1.0) (b 0.0)) (display (/ a b))) (newline)\n"
                                   "(define b 0.0)\n"
                                   "(display (/ 1.0 b)) (newline)\n"
                                   "(define n 0)\n"
                                   "(display (try (/ 1 n) (catch (e) e))) (newline)\n"
                                   "(display (r 1.0 (- 0.5 0.5))) (newline)\n"
                                   "(define (f v) (/ (vector-ref v 0) (vector-ref v 1)))\n"
                                   "(display (f (vector 3.0 0.0))) (newline)\n"
                                   "(define x (r 1.0 0.0))\n"
                                   "(display (number? x)) (display (= x 0)) (newline)\n"
                                   "(display (try (/ 1 0) (catch (e) e))) (newline)\n"
                                   "(display (/ 1.0 0.0)) (newline)\n"
                                   "(display (/ -0.001 0.0015)) (newline)\n"
                                   "(display \"after\") (newline)\n"),
                  "4461: write soft script");
            auto soft_run = run_aura_file(bin, soft, "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0");
            CHECK(soft_run.rc == 0, "4461: soft face exits 0");
            CHECK(soft_run.out.find("\n0\n") == std::string::npos, "4461: soft face no silent 0");
            CHECK(soft_run.out.find("inf\n-inf\nnan\n") == 0, "4461: soft float IEEE");
            CHECK(soft_run.out.find("/: division by zero") != std::string::npos,
                  "4461: soft integer zero is an error");
            CHECK(soft_run.out.find("#t#f") != std::string::npos,
                  "4461: inf is a number and not 0");
            CHECK(soft_run.out.find("after") != std::string::npos, "4461: soft continues");
            CHECK(soft_run.err.find("division by zero") == std::string::npos,
                  "4461: soft does not abort with a diagnostic");

            const char* lit = "/tmp/aura-4461-lit.aura";
            CHECK(write_text(lit, "(display (/ 1.0 0.0)) (newline)\n"
                                  "(display (/ -1.0 0.0)) (newline)\n"
                                  "(display (/ 0.0 0.0)) (newline)\n"
                                  "(display (try (/ 1 0) (catch (e) e))) (newline)\n"
                                  "(define b 0.0)\n"
                                  "(display (/ 1.0 b)) (newline)\n"
                                  "(define n 0)\n"
                                  "(display (try (/ 1 n) (catch (e) e))) (newline)\n"
                                  "(display (/ 1.0 (- 0.5 0.5))) (newline)\n"
                                  "(display (/ -0.001 0.0015)) (newline)\n"
                                  "(display \"after\") (newline)\n"),
                  "4461: write literal script");
            auto lit_run = run_aura_file(bin, lit, "");
            CHECK(lit_run.rc == 0, "4461: default face exits 0");
            CHECK(lit_run.out.find("inf\n-inf\nnan\n") == 0, "4461: literal float IEEE");
            CHECK(lit_run.out.find("/: division by zero") != std::string::npos,
                  "4461: literal integer zero matches the binding");
            CHECK(lit_run.out.find("after") != std::string::npos, "4461: default continues");
            CHECK(lit_run.err.find("error:") == std::string::npos,
                  "4461: default face does not abort");
        }
    }

    // Same results on the soft face (tree walker) and the default face (IR).
    {
        const auto bin = aura_binary();
        CHECK(!bin.empty(), "4462: aura binary");
        if (!bin.empty()) {
            // Issue #4482: symmetric fixnums; overflow is an error value.
            auto fmt = [](double d) { return aura::compiler::format_float_shortest(d); };
            const std::string two62 = fmt(4611686018427387904.0);
            const std::string two63 = fmt(9223372036854775808.0);
            // Overflow cases are checked in-process above (error values).
            std::string expect = "4611686018427387902\n" + two62 + "\n" + two62 +
                                 "\n-2305843009213693952\n-4611686018427387904\n"
                                 "-4611686018427387904\n-5000000000000000\n"
                                 "4611686018427387903\n4294967296\n" +
                                 two62 + "\n" + two63 +
                                 "\n2305843009213693952\n-3000000000000000000\nafter\n";
            const char* path = "/tmp/aura-4462.aura";
            CHECK(write_text(path, "(define c 2305843009213693951)\n"
                                   "(display (+ c c)) (newline)\n"
                                   "(display 4611686018427387904) (newline)\n"
                                   "(display (string->number \"4611686018427387904\")) (newline)\n"
                                   "(define d -2305843009213693952)\n"
                                   "(display d) (newline)\n"
                                   "(display (+ d d)) (newline)\n"
                                   "(display -4611686018427387904) (newline)\n"
                                   "(display -5000000000000000) (newline)\n"
                                   "(define a 4294967296)\n"
                                   "(display 4611686018427387903) (newline)\n"
                                   "(display '4294967296) (newline)\n"
                                   "(display '4611686018427387904) (newline)\n"
                                   "(display 9223372036854775808) (newline)\n"
                                   "(define p 1073741824)\n"
                                   "(define q 2147483648)\n"
                                   "(display (* p q)) (newline)\n"
                                   "(define (m x y) (* x y))\n"
                                   "(display (m -3 1000000000000000000)) (newline)\n"
                                   "(display \"after\") (newline)\n"),
                  "4462: write script");
            auto soft = run_aura_file(bin, path, "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0",
                                      "/tmp/aura-4462-soft-out.txt", "/tmp/aura-4462-soft-err.txt");
            if (soft.out != expect || soft.rc != 0)
                std::println(std::cerr, "soft rc {} out [{}] err [{}]", soft.rc, soft.out,
                             soft.err);
            CHECK(soft.rc == 0, "4462: soft face exits 0");
            CHECK(soft.out == expect, "4482: soft face prints exact ints and overflow errors");
            CHECK(soft.err.find("AURA CRASH") == std::string::npos,
                  "4462: soft face does not abort");
            auto def = run_aura_file(bin, path, "", "/tmp/aura-4462-def-out.txt",
                                     "/tmp/aura-4462-def-err.txt");
            if (def.out != expect || def.rc != 0)
                std::println(std::cerr, "default rc {} out [{}] err [{}]", def.rc, def.out,
                             def.err);
            CHECK(def.rc == 0, "4462: default face exits 0");
            CHECK(def.out == expect, "4462: default face agrees with the soft face");
            CHECK(def.err.find("AURA CRASH") == std::string::npos,
                  "4462: default face does not abort");
        }
    }

    // Issue #4463: a non-number is a type error on both faces. The legacy
    // stderr line stays. The value is an error, not a coerced int.
    {
        const auto bin = aura_binary();
        CHECK(!bin.empty(), "4463: aura binary");
        if (!bin.empty()) {
            const char* path = "/tmp/aura-4463.aura";
            CHECK(write_text(path, "(display (+ 1 #f)) (newline)\n"
                                   "(display (+ 1 #t)) (newline)\n"
                                   "(display (* 1.0 \"2.5\")) (newline)\n"
                                   "(display (* 1.0 \"0.95\")) (newline)\n"
                                   "(display (+ 1 \"12abc\")) (newline)\n"
                                   "(display (- 10 \"4\")) (newline)\n"
                                   "(display (abs \"-3\")) (newline)\n"
                                   "(display (floor \"2.7\")) (newline)\n"
                                   "(display (exp #f)) (newline)\n"
                                   "(display (< 0 #f)) (newline)\n"
                                   "(display (+ 1 \"abc\")) (newline)\n"
                                   "(display (+ 2 3)) (newline)\n"
                                   "(display (* 1.0 2)) (newline)\n"
                                   "(display (= 1 1)) (newline)\n"
                                   "(display (try (+ 1 \"abc\") (catch (e) e))) (newline)\n"
                                   "(define (scale x) (* x 1.5))\n"
                                   "(display (scale #f)) (newline)\n"
                                   "(display (scale \"2\")) (newline)\n"
                                   "(define v (make-vector 2 #f))\n"
                                   "(display (+ (vector-ref v 0) 1)) (newline)\n"
                                   "(display \"end\") (newline)\n"),
                  "4463: write script");
            const std::string expect = "<unknown>\n<unknown>\n<unknown>\n<unknown>\n<unknown>\n"
                                       "<unknown>\n<unknown>\n<unknown>\n<unknown>\n<unknown>\n"
                                       "<unknown>\n5\n2.0\n#t\n"
                                       "type mismatch — expected Int, got String 'abc'\n"
                                       "<unknown>\n<unknown>\n<unknown>\nend\n";
            auto check_face = [&](const char* face, const char* outp, const char* errp,
                                  const char* label) {
                auto run = run_aura_file(bin, path, face, outp, errp);
                if (run.out != expect || run.rc != 0)
                    std::println(std::cerr, "{} rc {} out [{}] err [{}]", label, run.rc, run.out,
                                 run.err);
                CHECK(run.rc == 0, std::string(label) + " exits 0");
                CHECK(run.out == expect, std::string(label) + " does not coerce");
                CHECK(run.err.find("error: type mismatch — expected Int, got String 'abc'") !=
                          std::string::npos,
                      std::string(label) + " string 'abc'");
                CHECK(run.err.find("error: type mismatch — expected Int, got String '2.5'") !=
                          std::string::npos,
                      std::string(label) + " string '2.5'");
                CHECK(run.err.find("error: type mismatch — expected Int, got String '-3'") !=
                          std::string::npos,
                      std::string(label) + " string '-3'");
                CHECK(run.err.find("error: type mismatch — expected Int, got Bool") !=
                          std::string::npos,
                      std::string(label) + " bool");
                CHECK(run.err.find("AURA CRASH") == std::string::npos,
                      std::string(label) + " does not abort");
            };
            check_face("AURA_SANDBOX=off AURA_PIPELINE_STRICT=0", "/tmp/aura-4463-soft-out.txt",
                       "/tmp/aura-4463-soft-err.txt", "4463: soft");
            check_face("", "/tmp/aura-4463-def-out.txt", "/tmp/aura-4463-def-err.txt",
                       "4463: default");
        }
    }

    // Issue #4469: round halves to even, integer? accepts integral floats,
    // and the R7RS names exist.
    {
        auto expect_int = [&](const char* expr, std::int64_t want, const char* msg) {
            auto r = cs.eval(expr);
            CHECK(r && is_int(*r) && as_int(*r) == want, msg);
        };
        auto expect_bool = [&](const char* expr, bool want, const char* msg) {
            auto r = cs.eval(expr);
            CHECK(r && is_bool(*r) && aura::compiler::types::as_bool(*r) == want, msg);
        };
        auto expect_float = [&](const char* expr, double want, const char* msg) {
            auto r = cs.eval(expr);
            CHECK(r && is_float(*r) && as_float(*r) == want, msg);
        };
        expect_int("(round 0.5)", 0, "round 0.5 → 0");
        expect_int("(round -0.5)", 0, "round -0.5 → 0");
        expect_int("(round 1.5)", 2, "round 1.5 → 2");
        expect_int("(round -1.5)", -2, "round -1.5 → -2");
        expect_int("(round 2.5)", 2, "round 2.5 → 2");
        expect_int("(round -2.5)", -2, "round -2.5 → -2");
        expect_int("(round 2.4)", 2, "round 2.4 → 2");
        expect_int("(round 2.6)", 3, "round 2.6 → 3");
        expect_int("(round -2.6)", -3, "round -2.6 → -3");
        expect_int("(round 2)", 2, "round of an exact int stays exact");
        expect_bool("(integer? 2)", true, "integer? 2");
        expect_bool("(integer? 2.0)", true, "integer? 2.0");
        expect_bool("(integer? 2.5)", false, "integer? 2.5");
        expect_bool("(integer? (floor 2.7))", true, "integer? (floor 2.7)");
        expect_bool("(integer? (round 2.4))", true, "integer? (round 2.4)");
        expect_int("(inexact->exact 2.0)", 2, "inexact->exact 2.0");
        expect_int("(exact 2.0)", 2, "exact aliases inexact->exact");
        expect_float("(exact->inexact 2)", 2.0, "exact->inexact 2");
        expect_float("(inexact 2)", 2.0, "inexact aliases exact->inexact");
        expect_bool("(exact? 2)", true, "exact? 2");
        expect_bool("(exact? 2.0)", false, "exact? 2.0");
        expect_bool("(inexact? 2.0)", true, "inexact? 2.0");
        expect_bool("(inexact? 2)", false, "inexact? 2");
        auto tr = cs.eval("(truncate -2.9)");
        auto tru = cs.eval("(trunc -2.9)");
        CHECK(tr && tru && is_float(*tr) && is_float(*tru) && as_float(*tr) == as_float(*tru),
              "truncate aliases trunc");
        auto ce = cs.eval("(ceiling -2.1)");
        auto cei = cs.eval("(ceil -2.1)");
        CHECK(ce && cei && is_float(*ce) && is_float(*cei) && as_float(*ce) == as_float(*cei),
              "ceiling aliases ceil");

        const auto bin = aura_binary();
        CHECK(!bin.empty(), "4469: aura binary");
        if (!bin.empty()) {
            const char* path = "/tmp/aura-4469-repro.aura";
            const std::string expect = "0\n2\n-2\n#t\n#t\n#t\n#t\n2\n2.0\n";
            CHECK(write_text(path, "(display (round 0.5)) (newline)\n"
                                   "(display (round 2.5)) (newline)\n"
                                   "(display (round -2.5)) (newline)\n"
                                   "(display (integer? 2)) (newline)\n"
                                   "(display (integer? 2.0)) (newline)\n"
                                   "(display (integer? (round 2.4))) (newline)\n"
                                   "(display (integer? (floor 2.7))) (newline)\n"
                                   "(display (inexact->exact 2.0)) (newline)\n"
                                   "(display (exact->inexact 2)) (newline)\n"),
                  "4469: write repro");
            auto check_face = [&](const char* face, const char* outp, const char* errp,
                                  const char* label) {
                auto run = run_aura_file(bin, path, face, outp, errp);
                if (run.out != expect || run.rc != 0)
                    std::println(std::cerr, "{} rc {} out [{}] err [{}]", label, run.rc, run.out,
                                 run.err);
                CHECK(run.rc == 0, std::string(label) + " exits 0");
                CHECK(run.out == expect, std::string(label) + " prints the R7RS block");
            };
            check_face("AURA_SANDBOX=off AURA_PIPELINE_STRICT=0", "/tmp/aura-4469-soft-out.txt",
                       "/tmp/aura-4469-soft-err.txt", "4469: soft");
            check_face("", "/tmp/aura-4469-def-out.txt", "/tmp/aura-4469-def-err.txt",
                       "4469: default");
        }
    }

    // Issue #4470: eqv? is a primitive (same exactness, then identity).
    // memv / assv walk with that predicate. Soft is the tree-walker;
    // force-soa lowers the procedure and must agree.
    {
        auto eqv = [&](const char* expr, bool want, const char* label) {
            auto r = cs.eval(expr);
            CHECK(r && is_bool(*r) && as_bool(*r) == want, label);
        };
        eqv("(eqv? 2 2)", true, "eqv? 2 2");
        eqv("(eqv? 2 2.0)", false, "eqv? 2 2.0");
        eqv("(eqv? 1.5 1.5)", true, "eqv? 1.5 1.5");
        eqv("(eqv? 1.5 1.6)", false, "eqv? 1.5 1.6");
        eqv("(eqv? 'a 'a)", true, "eqv? symbol a");
        eqv("(eqv? 'a 'b)", false, "eqv? symbol a b");
        eqv("(eqv? #\\a #\\a)", true, "eqv? char a");
        eqv("(eqv? #\\a #\\b)", false, "eqv? char a b");
        auto hit = cs.eval("(memv 2 (list 1 2 3))");
        CHECK(hit && is_pair(*hit), "memv 2 hits");
        auto miss = cs.eval("(memv 2.0 (list 1 2 3))");
        CHECK(miss && is_bool(*miss) && !aura::compiler::types::as_bool(*miss), "memv 2.0 misses an int (#f, #4491)");
        auto alist = cs.eval("(cdr (assv 'b (list (cons 'a 1) (cons 'b 2))))");
        CHECK(alist && is_int(*alist) && as_int(*alist) == 2, "assv 'b yields 2");
        auto no = cs.eval("(assv 'z (list (cons 'a 1)))");
        CHECK(no && is_bool(*no) && !aura::compiler::types::as_bool(*no), "assv miss is #f (#4491)");

        const auto bin = aura_binary();
        CHECK(!bin.empty(), "4470: aura binary");
        if (!bin.empty()) {
            const char* repro = "/tmp/aura-4470-repro.aura";
            const std::string repro_expect = "#t\n#t\nafter\n";
            CHECK(write_text(repro, "(display (eq? 'a 'a)) (newline)\n"
                                    "(display (eqv? 2 2)) (newline)\n"
                                    "(display \"after\") (newline)\n"),
                  "4470: write repro");
            auto check_repro = [&](const char* face, const char* outp, const char* errp,
                                   const char* label) {
                auto run = run_aura_file(bin, repro, face, outp, errp);
                if (run.out != repro_expect || run.rc != 0)
                    std::println(std::cerr, "{} rc {} out [{}] err [{}]", label, run.rc, run.out,
                                 run.err);
                CHECK(run.rc == 0, std::string(label) + " exits 0");
                CHECK(run.out == repro_expect, std::string(label) + " prints #t #t after");
            };
            check_repro("AURA_SANDBOX=off AURA_PIPELINE_STRICT=0", "/tmp/aura-4470-soft-out.txt",
                        "/tmp/aura-4470-soft-err.txt", "4470: soft repro");

            const char* agree = "/tmp/aura-4470-agree.aura";
            const std::string agree_expect =
                "#t\n#f\n#t\n#f\n#t\n#f\n#t\n#f\n#t\n#f\n1.5\n#t\n2\n#f\n";
            CHECK(write_text(agree,
                             "(define (ev a b) (eqv? a b))\n"
                             "(display (ev 2 2)) (newline)\n"
                             "(display (ev 2 2.0)) (newline)\n"
                             "(display (ev 1.5 1.5)) (newline)\n"
                             "(display (ev 1.5 1.6)) (newline)\n"
                             "(display (ev 'a 'a)) (newline)\n"
                             "(display (ev 'a 'b)) (newline)\n"
                             "(display (ev #\\a #\\a)) (newline)\n"
                             "(display (ev #\\a #\\b)) (newline)\n"
                             "(display (if (memv 2 (list 1 2 3)) #t #f)) (newline)\n"
                             "(display (if (memv 2.0 (list 1 2 3)) #t #f)) (newline)\n"
                             "(display (car (memv 1.5 (list 1.0 1.5)))) (newline)\n"
                             "(display (if (assv 'b (list (cons 'a 1) (cons 'b 2))) #t #f)) "
                             "(newline)\n"
                             "(display (cdr (assv 'b (list (cons 'a 1) (cons 'b 2))))) (newline)\n"
                             "(display (if (assv 'z (list (cons 'a 1))) #t #f)) (newline)\n"),
                  "4470: write agreement");
            auto check_agree = [&](const char* face, const char* outp, const char* errp,
                                   const char* label) {
                auto run = run_aura_file(bin, agree, face, outp, errp);
                if (run.out != agree_expect || run.rc != 0)
                    std::println(std::cerr, "{} rc {} out [{}] err [{}]", label, run.rc, run.out,
                                 run.err);
                CHECK(run.rc == 0, std::string(label) + " exits 0");
                CHECK(run.out == agree_expect, std::string(label) + " tree and IR agree");
            };
            check_agree("AURA_SANDBOX=off AURA_PIPELINE_STRICT=0",
                        "/tmp/aura-4470-agree-soft-out.txt", "/tmp/aura-4470-agree-soft-err.txt",
                        "4470: soft agree");
            check_agree("AURA_SANDBOX=off AURA_PIPELINE_STRICT=force-soa",
                        "/tmp/aura-4470-agree-soa-out.txt", "/tmp/aura-4470-agree-soa-err.txt",
                        "4470: force-soa agree");
        }
    }

    // Issue #4483: a c-func -> Int returning a C long outside the fixnum
    // domain must not abort the process; it promotes like other
    // out-of-domain integers (#4462).
    {
        const auto bin = aura_binary();
        const char* cpath = "/tmp/aura-4483-lib.c";
        const char* sopath = "/tmp/aura-4483-lib.so";
        const bool have_cc =
            write_text(cpath,
                       "long s_min(long a) { (void)a; return (-9223372036854775807L - 1); }\n"
                       "long s_big(long a) { (void)a; return 1L << 62; }\n") &&
            std::system(
                std::format("cc -shared -fPIC -o {} {} 2>/dev/null", sopath, cpath).c_str()) == 0;
        if (!bin.empty() && have_cc) {
            const char* src = "/tmp/aura-4483.aura";
            CHECK(write_text(src, "(require \"std/ffi\" all:)\n"
                                  "(define lib (c-load \"/tmp/aura-4483-lib.so\"))\n"
                                  "(define f (c-func lib \"s_min\" \"(Int) -> Int\"))\n"
                                  "(define g (c-func lib \"s_big\" \"(Int) -> Int\"))\n"
                                  "(display (number? (f 1))) (newline)\n"
                                  "(display (> (g 1) 0)) (newline)\n"
                                  "(display \"after\") (newline)\n"),
                  "4483: write script");
            auto run = run_aura_file(bin, src, "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0",
                                     "/tmp/aura-4483-out.txt", "/tmp/aura-4483-err.txt");
            CHECK(run.rc == 0, "4483: out-of-range c-func Int result does not abort");
            CHECK(run.out == "#t\n#t\nafter\n", "4483: out-of-range results promote");
        } else {
            std::println("  SKIP: 4483 (no aura binary or cc)");
        }
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("arithmetic int64 safety: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
