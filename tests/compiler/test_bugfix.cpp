// Issue #957/#968/#982/#984 (#1978 renamed): issue# moved from filename to header.
// test_bugfix_968_984.cpp — Issues #968–#984 bugfix regression

#include "test_harness.hpp"

#include "compiler/value_tags.h" // Issue #4532: ffi closure id namespace helpers

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ffi_primitives;
import aura.compiler.evaluator_pure;

using aura::compiler::CompilerService;
using aura::compiler::FFIRuntime;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_error;
using aura::compiler::types::is_ffi_closure_id;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;

// Issue #4531 fixtures: local C functions standing in for c-func targets.
static double ffi4531_si(const char* p, std::int64_t n) {
    return static_cast<double>(p[0]) + static_cast<double>(n);
}
static double ffi4531_sf(const char* p, double k) {
    return static_cast<double>(p[0]) * k;
}
static double ffi4531_i(std::int64_t a) {
    return static_cast<double>(a) * 0.5;
}
static std::int64_t ffi4531_if(std::int64_t a, double k) {
    return a + static_cast<std::int64_t>(k);
}
static double ffi4531_z() {
    return 2.5;
}
static std::int64_t ffi4531_len(const char* p) {
    return static_cast<std::int64_t>(p[0]);
}
// Issue #4553 fixtures: more than 6 integer-class slots (stack-passed past
// rdi..r9) and mixed classes past the old 6-argument total.
static std::int64_t ffi4553_pos8(std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t d,
                                 std::int64_t e, std::int64_t f, std::int64_t g, std::int64_t h) {
    return a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * f + 1000000 * g + 10000000 * h;
}
static double ffi4553_mix(std::int64_t a, double x, std::int64_t b, double y, std::int64_t c,
                          double z, std::int64_t d, double w) {
    return static_cast<double>(a) + 10 * x + 100 * static_cast<double>(b) + 1000 * y +
           10000 * static_cast<double>(c) + 100000 * z + 1000000 * static_cast<double>(d) +
           10000000 * w;
}
static std::int64_t ffi4553_sum16(std::int64_t a0, std::int64_t a1, std::int64_t a2,
                                  std::int64_t a3, std::int64_t a4, std::int64_t a5,
                                  std::int64_t a6, std::int64_t a7, std::int64_t a8,
                                  std::int64_t a9, std::int64_t a10, std::int64_t a11,
                                  std::int64_t a12, std::int64_t a13, std::int64_t a14,
                                  std::int64_t a15, double f0, double f7) {
    return a0 + 2 * a1 + 3 * a2 + 4 * a3 + 5 * a4 + 6 * a5 + 7 * a6 + 8 * a7 + 9 * a8 + 10 * a9 +
           11 * a10 + 12 * a11 + 13 * a12 + 14 * a13 + 15 * a14 + 16 * a15 +
           static_cast<std::int64_t>(f0 * 100) + static_cast<std::int64_t>(f7 * 1000);
}

int run_test_bugfix() {
    // #982: parse_ffi_sig rejects missing ')'
    {
        int ret = 0;
        std::vector<int> args;
        std::string err;
        CHECK(!FFIRuntime::parse_ffi_sig("(Int -> Int", ret, args, &err), "missing ) rejected");
        CHECK(FFIRuntime::parse_ffi_sig("(Int) -> Int", ret, args, &err),
              "well-formed sig accepted");
        CHECK(ret == 1, "ret type Int");
    }

    // #968: snapshot populates function metrics fields (no crash)
    {
        CompilerService cs;
        auto snap = cs.snapshot();
        // functions may be empty without JIT activity; just ensure path works
        for (auto& f : snap.functions) {
            CHECK(!f.name.empty(), "fn metrics name set");
            (void)f.total_calls;
            (void)f.hit_rate;
            (void)f.specialized_for;
        }
        CHECK(true, "snapshot() ok");
    }

    // #957/basic: defuse_version still works
    {
        CompilerService cs;
        auto v = cs.evaluator().defuse_version();
        CHECK(v >= 0, "defuse_version readable");
        (void)v;
    }

    // Smoke: arithmetic still works after typechecker changes
    {
        CompilerService cs;
        auto r = cs.eval("(+ 40 2)");
        CHECK(r && is_int(*r) && as_int(*r) == 42, "(+ 40 2) = 42");
    }

    // Issue #4531: the class-split FFI dispatcher. SysV assigns integer args
    // (rdi..r9) and SSE args (xmm0..7) independently, so the int-first
    // prototype ffi_call_by_class calls through is register-compatible with
    // any declared interleaving of the same (ni, nd) shape, and a Float
    // result comes from xmm0 whatever the argument classes are. The old
    // any_float switch read Float returns from rax and sent String/Int
    // slots through xmm.
    {
        using aura::compiler::pure::ffi_call_by_class;
        const std::int64_t p_s = reinterpret_cast<std::int64_t>("A"); // p[0] == 'A' == 65
        // (String Int) -> Float: both classes, Float return from xmm0.
        {
            const std::int64_t ic[2] = {p_s, 3};
            CHECK(ffi_call_by_class<double>(reinterpret_cast<void*>(ffi4531_si), ic, nullptr, 2,
                                            0) == 68.0,
                  "4531: (String Int) -> Float reads xmm0");
        }
        // (String Float) -> Float: mixed classes, each in its own register
        // class (the old double trampoline sent the pointer through xmm).
        {
            const std::int64_t ic[1] = {p_s};
            const double dc[1] = {2.0};
            CHECK(ffi_call_by_class<double>(reinterpret_cast<void*>(ffi4531_sf), ic, dc, 1, 1) ==
                      130.0,
                  "4531: (String Float) -> Float keeps the pointer in rdi");
        }
        // (Int) -> Float: Float return without a Float argument (the old
        // int trampoline read rax).
        {
            const std::int64_t ic[1] = {3};
            CHECK(ffi_call_by_class<double>(reinterpret_cast<void*>(ffi4531_i), ic, nullptr, 1,
                                            0) == 1.5,
                  "4531: (Int) -> Float reads xmm0");
        }
        // (Int Float) -> Int: integer return from rax with a Float slot.
        {
            const std::int64_t ic[1] = {2};
            const double dc[1] = {4.0};
            CHECK(ffi_call_by_class<std::int64_t>(reinterpret_cast<void*>(ffi4531_if), ic, dc, 1,
                                                  1) == 6,
                  "4531: (Int Float) -> Int reads rax");
        }
        // () -> Float: no args at all.
        CHECK(ffi_call_by_class<double>(reinterpret_cast<void*>(ffi4531_z), nullptr, nullptr, 0,
                                        0) == 2.5,
              "4531: () -> Float reads xmm0");
        // String slot lands in rdi (the callee dereferences it).
        {
            const std::int64_t ic[1] = {p_s};
            CHECK(ffi_call_by_class<std::int64_t>(reinterpret_cast<void*>(ffi4531_len), ic, nullptr,
                                                  1, 0) == 65,
                  "4531: String slot passes a live pointer");
        }
    }

    // Issue #4553: the dispatcher used to stop at ni + nd <= 6 (and the
    // marshaler silently dropped arguments past the 6th). Integer-class
    // slots past the registers go on the stack in declared order; Floats
    // stay in xmm0..7, so the int-first prototype stays compatible.
    {
        using aura::compiler::pure::ffi_call_by_class;
        const std::int64_t ic8[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        CHECK(ffi_call_by_class<std::int64_t>(reinterpret_cast<void*>(ffi4553_pos8), ic8, nullptr,
                                              8, 0) == 87654321,
              "4553: 8 Int slots arrive in order (7th and 8th on the stack)");
        const std::int64_t ic4[4] = {1, 3, 5, 7};
        const double dc4[4] = {2.0, 4.0, 6.0, 8.0};
        CHECK(ffi_call_by_class<double>(reinterpret_cast<void*>(ffi4553_mix), ic4, dc4, 4, 4) ==
                  87654321.0,
              "4553: 4 Int + 4 Float interleaved (8 args) arrive in order");
        std::int64_t ic16[16];
        for (int i = 0; i < 16; ++i)
            ic16[i] = 1;
        const double dc2[2] = {0.5, 0.25};
        CHECK(ffi_call_by_class<std::int64_t>(reinterpret_cast<void*>(ffi4553_sum16), ic16, dc2, 16,
                                              2) == 136 + 50 + 250,
              "4553: 16 integer-class slots + 2 Float arrive in order");
        CHECK(ffi_call_by_class<std::int64_t>(reinterpret_cast<void*>(ffi4553_pos8), ic8, nullptr,
                                              17, 0) == 0,
              "4553: a shape past the slot limits is not called");
    }

    // Issue #4554: str_bufs was a growing std::vector<std::string>; the
    // second String argument's push_back moved the first, and an SSO
    // (<= 15 byte) string's bytes move with the object, so s_vals[0]
    // pointed into freed storage. Each argument now owns a fixed slot.
    // Issue #4551: tags are checked against the declared kinds.
    {
        using aura::compiler::pure::ffi_marshal_args_pure;
        const std::vector<std::string> heap{"short", "other", "a-long-string-over-15-bytes"};
        const std::vector<int> ss{3, 3, 3};
        const std::array<EvalValue, 3> sargs{make_string(0), make_string(1), make_string(2)};
        const auto m = ffi_marshal_args_pure(sargs, ss, heap, {});
        CHECK(m.ok() && m.ni == 3 && m.nd == 0, "4554: three String slots marshal");
        bool live = true;
        for (std::size_t i = 0; i < 3; ++i)
            live = live && m.s_vals[i] == m.str_bufs[i].c_str() &&
                   std::string_view(m.s_vals[i]) == heap[i] &&
                   m.ic[i] == reinterpret_cast<std::int64_t>(m.s_vals[i]);
        CHECK(live, "4554: every String pointer names its own live buffer (SSO included)");

        const std::vector<int> isig{1};
        const std::array<EvalValue, 1> sarg{make_string(0)};
        const auto bad_i = ffi_marshal_args_pure(sarg, isig, heap, {});
        CHECK(!bad_i.ok() && bad_i.bad_arg == 0 && bad_i.bad_kind == 1,
              "4551: String into an Int slot is a mismatch");
        const std::vector<int> ssig{1, 3};
        const std::array<EvalValue, 2> iarg{make_int(1), make_int(5)};
        const auto bad_s = ffi_marshal_args_pure(iarg, ssig, heap, {});
        CHECK(!bad_s.ok() && bad_s.bad_arg == 1 && bad_s.bad_kind == 3,
              "4551: Int into a String slot is a mismatch (was dereferenced)");
        const auto arity = ffi_marshal_args_pure(iarg, isig, heap, {});
        CHECK(!arity.ok() && arity.arity_mismatch, "4551: argument count must match");
        const std::vector<int> fsig{2};
        const std::array<EvalValue, 1> farg{make_int(3)};
        const auto widen = ffi_marshal_args_pure(farg, fsig, heap, {});
        CHECK(widen.ok() && widen.nd == 1 && widen.dc[0] == 3.0, "4551: Int widens into Float");
    }

    // Issues #4551 / #4553 / #4554 end to end: a cc-built library loaded
    // with c-load. Before the fixes: (kid "x") returned 1, (count7 7 x8)
    // returned 6, the 8-argument mixed call returned garbage, a short first
    // String argument read freed memory once the callee allocated, and
    // (klen 5 1) crashed the process (SIGSEGV).
    {
        const char* cpath = "/tmp/aura-cfunc-4551-lib.c";
        const char* sopath = "/tmp/aura-cfunc-4551-lib.so";
        bool have_cc = false;
        {
            std::ofstream c(cpath);
            c << "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
                 "long kid(long a) { return a + 1; }\n"
                 "long klen(const char *s, long n) { long h = 0; for (long i = 0; i < n; i++) "
                 "h += (unsigned char)s[i]; return h; }\n"
                 "long count7(long a, long b, long c, long d, long e, long f, long g, long h) "
                 "{ return (a == 7) + (b == 7) + (c == 7) + (d == 7) + (e == 7) + (f == 7) + "
                 "(g == 7) + (h == 7); }\n"
                 "double mix(long a, double x, long b, double y, long c, double z, long d, "
                 "double w) { return a + 10*x + 100*b + 1000*y + 10000*c + 100000*z + "
                 "1000000*d + 10000000*w; }\n"
                 "static char out[256];\n"
                 "const char *join2(const char *a, const char *b) { void *keep[16]; "
                 "for (int i = 0; i < 16; i++) { keep[i] = malloc(32); memset(keep[i], 0x58, "
                 "31); ((char *)keep[i])[31] = 0; } snprintf(out, sizeof out, \"%s|%s\", a, b); "
                 "for (int i = 0; i < 16; i++) free(keep[i]); return out; }\n";
            have_cc = static_cast<bool>(c);
        }
        have_cc =
            have_cc &&
            std::system(
                std::format("cc -O2 -shared -fPIC -o {} {} 2>/dev/null", sopath, cpath).c_str()) ==
                0;
        if (have_cc) {
            CompilerService cs;
            auto ok = cs.eval(std::format(
                "(begin (require \"std/ffi\" all:)"
                " (define lib4551 (c-load \"{}\"))"
                " (define kid4551 (c-func lib4551 \"kid\" \"(Int) -> Int\"))"
                " (define klen4551 (c-func lib4551 \"klen\" \"(String Int) -> Int\"))"
                " (define c7-4553 (c-func lib4551 \"count7\" \"(Int Int Int Int Int Int Int "
                "Int) -> Int\"))"
                " (define mix4553 (c-func lib4551 \"mix\" \"(Int Float Int Float Int Float Int "
                "Float) -> Float\"))"
                " (define join4554 (c-func lib4551 \"join2\" \"(String String) -> String\"))"
                " #t)",
                sopath));
            CHECK(ok.has_value(), "4551: library and c-funcs load");
            auto truthy = [&](const char* src, const char* label) {
                auto r = cs.eval(src);
                CHECK(r && is_bool(*r) && as_bool(*r), label);
            };
            truthy("(= (kid4551 41) 42)", "4551: well-typed Int call");
            truthy("(equal? (try (kid4551 \"x\") (catch (e) \"caught\")) \"caught\")",
                   "4551: String into Int slot raises a catchable error");
            truthy("(equal? (try (kid4551 1.5) (catch (e) \"caught\")) \"caught\")",
                   "4551: Float into Int slot raises (no silent truncation)");
            truthy("(equal? (try (klen4551 5 1) (catch (e) \"caught\")) \"caught\")",
                   "4551: Int into String slot raises instead of SIGSEGV");
            truthy("(= (klen4551 \"ab\" 2) 195)", "4551: well-typed String call");
            truthy("(= (c7-4553 7 7 7 7 7 7 7 7) 8)", "4553: all 8 Int arguments arrive");
            truthy("(= (mix4553 1 2.0 3 4.0 5 6.0 7 8.0) 87654321.0)",
                   "4553: 4 Int + 4 Float arguments arrive in order");
            truthy("(equal? (join4554 \"short\" \"other\") \"short|other\")",
                   "4554: short first String argument survives a callee allocation");
            truthy("(equal? (join4554 \"k205\" \"a-long-string-over-15-bytes\") "
                   "\"k205|a-long-string-over-15-bytes\")",
                   "4554: short String before a long one");
        } else {
            std::println("  SKIP: 4551/4553/4554 end to end (no cc)");
        }
    }

    // Issue #4532: c-func ids used bit 63, which make_ref's pool shift
    // strips — the raw ffi index then collided with TW closure ids and
    // apply_closure's `cid < func_count()` stole small-id closures into
    // the C function once enough c-funcs were registered. FFI ids now
    // carry their own namespace bit and dispatch tests the bit explicitly.
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(define f4532 (lambda (x) (* x 2)))").has_value(),
              "4532: early TW closure defined");
        auto f4532 = cs.eval("f4532");
        CHECK(is_closure(*f4532), "4532: closure value resolvable");
        CHECK(!is_ffi_closure_id(as_closure_id(*f4532)),
              "4532: TW closure is outside the ffi namespace");

        CHECK(cs.eval("(require \"std/ffi\")").has_value(), "4532: require std/ffi");
        auto cfunc_fn = ev.primitives().lookup("c-func");
        auto list_fn = ev.primitives().lookup("list");
        auto apply_fn = ev.primitives().lookup("apply");
        CHECK(cfunc_fn && list_fn && apply_fn, "4532: c-func/list/apply present");
        auto name_s = make_string(static_cast<std::uint64_t>(ev.push_string_heap("strlen")));
        auto sig_s =
            make_string(static_cast<std::uint64_t>(ev.push_string_heap("(String) -> Int")));
        EvalValue proc4532;
        bool registered = true;
        for (int i = 0; i < 200 && registered; ++i) {
            auto r = (*cfunc_fn)({make_int(-1), name_s, sig_s});
            if (is_error(r) || !is_closure(r))
                registered = false;
            else
                proc4532 = r;
        }
        CHECK(registered, "4532: 200 c-func procedures registered");
        CHECK(is_closure(proc4532) && is_ffi_closure_id(as_closure_id(proc4532)),
              "4532: ffi namespace bit survives make_ref");
        // The small-id TW closure must still dispatch as a TW closure after
        // func_count() blew past its id.
        auto r42 = cs.eval("(f4532 21)");
        CHECK(r42 && is_int(*r42) && as_int(*r42) == 42,
              "4532: small-id closure survives 200 c-funcs");
        // The registered FFI proc still dispatches natively.
        auto arg_s = make_string(static_cast<std::uint64_t>(ev.push_string_heap("abcd")));
        auto arglist = (*list_fn)({arg_s});
        auto r_len = (*apply_fn)({proc4532, arglist});
        CHECK(is_int(r_len) && as_int(r_len) == 4,
              "4532: registered ffi proc still dispatches natively");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("bugfix #968–#984: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_bugfix();
}
#endif
