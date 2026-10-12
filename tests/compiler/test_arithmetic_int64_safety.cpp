// test_arithmetic_int64_safety.cpp — Issues #1150–#1156 Phase 1

#include "test_harness.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
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
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
using aura::compiler::types::is_int;
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
        CHECK(is_int(*r) || is_float(*r), "large * is int or float");
    }

    // Issue #4462: integers outside the real fixnum domain promote to float.
    // The domain is [-4999999999999999, 2^62-1]. -2^62 encodes as INT64_MIN
    // and is not a fixnum. 2^62 aborts make_int unless it is promoted.
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
        exact_int("4611686018427387903", kMax, "2^62-1 stays int");
        exact_int("-4999999999999999", -4999999999999999LL, "most negative fixnum stays int");
        exact_int("(+ 2305843009213693951 2305843009213693951)", 4611686018427387902LL,
                  "(+ c c) stays int");
        exact_int("(* 1073741824 2147483648)", 2305843009213693952LL, "2^30*2^31 stays int");
        exact_int("(string->number \"42\")", 42, "string->number 42 stays int");
        exact_int("(string->number \"9007199254740993\")", 9007199254740993LL,
                  "string->number keeps 2^53+1");
        exact_int("(string->number \"+7\")", 7, "string->number keeps a leading plus");
        auto bad = cs.eval("(string->number \"nope\")");
        CHECK(bad && is_bool(*bad) && !aura::compiler::types::as_bool(*bad),
              "string->number failure is #f");
        exact_float("4611686018427387904", static_cast<double>(kTwo62), "literal 2^62 is float");
        exact_float("-4611686018427387904", static_cast<double>(kNegTwo62),
                    "literal -2^62 is float");
        exact_float("-5000000000000000", -5000000000000000.0, "-5e15 is float");
        exact_float("-2305843009213693952", -2305843009213693952.0, "-2^61 is float");
        exact_float("(+ 2305843009213693951 2305843009213693951 2)", static_cast<double>(kTwo62),
                    "(+ c c 2) is float");
        exact_float("(* 2 (+ 2305843009213693951 1))", static_cast<double>(kTwo62),
                    "(* 2 (+ c 1)) is float");
        exact_float("(+ -2305843009213693952 -2305843009213693952)", static_cast<double>(kNegTwo62),
                    "(+ d d) is float");
        exact_float("(- 4611686018427387903)", -static_cast<double>(kMax),
                    "negation of 2^62-1 is float");
        exact_float("(* 4294967296 4294967296)", 18446744073709551616.0,
                    "literal 2^32*2^32 is 2^64");
        exact_float("(* 4611686018427387903 2)", static_cast<double>(kMax) * 2.0,
                    "(2^62-1)*2 promotes");
        exact_float("(string->number \"4611686018427387904\")", static_cast<double>(kTwo62),
                    "string->number 2^62 is float");
        exact_float("(string->number \"9223372036854775808\")", 9223372036854775808.0,
                    "string->number past int64 is float");
        exact_float("9223372036854775808", 9223372036854775808.0, "literal 2^63 is float");
        const double nine = static_cast<double>(9000000000000000000LL);
        exact_float("(+ 9000000000000000000 9000000000000000000)", nine + nine,
                    "literals past 2^62 promote");
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
            auto fmt = [](double d) {
                char b[64];
                std::snprintf(b, sizeof(b), "%g", d);
                return std::string(b);
            };
            const std::string two62 = fmt(4611686018427387904.0);
            const std::string neg62 = fmt(-4611686018427387904.0);
            const std::string two64 = fmt(18446744073709551616.0);
            const std::string neg61 = fmt(-2305843009213693952.0);
            const std::string two63 = fmt(9223372036854775808.0);
            std::string expect = "4611686018427387902\n" + two62 + "\n" + two62 + "\n" + two62 +
                                 "\n" + two62 + "\n" + neg61 + "\n" + neg62 + "\n" + neg62 + "\n" +
                                 neg62 + "\n" + two64 + "\n" + two64 + "\n" +
                                 "4611686018427387903\n4294967296\n" + two62 + "\n" + two63 +
                                 "\n2305843009213693952\n" + two63 + "\nafter\n";
            const char* path = "/tmp/aura-4462.aura";
            CHECK(write_text(path, "(define c 2305843009213693951)\n"
                                   "(display (+ c c)) (newline)\n"
                                   "(display (+ c c 2)) (newline)\n"
                                   "(display (* 2 (+ c 1))) (newline)\n"
                                   "(display 4611686018427387904) (newline)\n"
                                   "(display (string->number \"4611686018427387904\")) (newline)\n"
                                   "(define d -2305843009213693952)\n"
                                   "(display d) (newline)\n"
                                   "(display (+ d d)) (newline)\n"
                                   "(display (- (+ d d) 1)) (newline)\n"
                                   "(display -4611686018427387904) (newline)\n"
                                   "(display (* 4294967296 4294967296)) (newline)\n"
                                   "(define a 4294967296)\n"
                                   "(display (* a a)) (newline)\n"
                                   "(display 4611686018427387903) (newline)\n"
                                   "(display '4294967296) (newline)\n"
                                   "(display '4611686018427387904) (newline)\n"
                                   "(display 9223372036854775808) (newline)\n"
                                   "(define p 1073741824)\n"
                                   "(define q 2147483648)\n"
                                   "(display (* p q)) (newline)\n"
                                   "(display (* 4611686018427387903 2)) (newline)\n"
                                   "(display \"after\") (newline)\n"),
                  "4462: write script");
            auto soft = run_aura_file(bin, path, "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0",
                                      "/tmp/aura-4462-soft-out.txt", "/tmp/aura-4462-soft-err.txt");
            if (soft.out != expect || soft.rc != 0)
                std::println(std::cerr, "soft rc {} out [{}] err [{}]", soft.rc, soft.out,
                             soft.err);
            CHECK(soft.rc == 0, "4462: soft face exits 0");
            CHECK(soft.out == expect, "4462: soft face prints the promoted values");
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
                                       "<unknown>\n5\n2\n#t\n"
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

    if (::aura::test::g_failed)
        return 1;
    std::println("arithmetic int64 safety: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
