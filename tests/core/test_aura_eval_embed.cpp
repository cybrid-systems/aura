// @category: unit
// @reason: Issue #4946 — aura::eval links an in-process Aura evaluator into a
//          host C++ program. This binary links ONLY aura::eval: no
//          aura_test_objects source, no module import, no aura --serve child.
//
// AC1  create / label / destroy a session
// AC2  eval-string returns the printed value + a coarse type
// AC3  state persists across calls (define, then use)
// AC4  (require "std/moead" all:) resolves the stdlib and ask/tell run
// AC5  an Aura diagnostic is data: AURA_EVAL_ERROR + JSON, process survives
// AC6  aura_eval_file loads a file; a missing file is AURA_EVAL_IO_ERROR
// AC7  invalid arguments are refused, never abort
// AC8  one session tolerates concurrent callers (calls serialized)

#include "eval/aura_eval.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool ok, const char* what) {
    if (ok) {
        ++g_passed;
        std::printf("  PASS %s\n", what);
    } else {
        ++g_failed;
        std::printf("  FAIL %s\n", what);
    }
}

bool has(const char* haystack, const char* needle) {
    return haystack != nullptr && std::strstr(haystack, needle) != nullptr;
}

// Runs one source string and returns the record (never NULL unless the call
// itself was refused); the status arrives through result.
std::string run(aura_eval_session* s, const char* source, int* status) {
    char* rec = nullptr;
    const int rc = aura_eval_source(s, source, static_cast<size_t>(-1), &rec);
    std::string out = rec ? rec : "";
    aura_eval_string_free(rec);
    if (status)
        *status = rc;
    return out;
}

void test_session_lifecycle() {
    std::printf("[AC1] session create / label / destroy\n");
    aura_eval_session* s = aura_eval_session_create("unit");
    check(s != nullptr, "session created");
    check(s != nullptr && std::string(aura_eval_session_label(s)) == "unit", "label round-trips");
    aura_eval_session_destroy(s);
    aura_eval_session_destroy(nullptr); // no-op
    check(aura_eval_abi_version() != nullptr && std::string(aura_eval_abi_version()) == "1",
          "abi version is 1");
}

void test_eval_string(aura_eval_session* s) {
    std::printf("[AC2] eval-string\n");
    int st = AURA_EVAL_INTERNAL;
    const std::string rec = run(s, "(+ 1 2)", &st);
    check(st == AURA_EVAL_OK, "status ok");
    check(has(rec.c_str(), "\"status\":\"ok\""), "record says ok");
    check(has(rec.c_str(), "\"value\":\"3\""), "value is 3");
    check(has(rec.c_str(), "\"type\":\"int\""), "type is int");

    const std::string str = run(s, "(string-append \"ab\" \"cd\")", &st);
    check(st == AURA_EVAL_OK && has(str.c_str(), "\"value\":\"\\\"abcd\\\"\""),
          "string value is JSON-escaped");

    const std::string flt = run(s, "(* 1.5 2.0)", &st);
    check(st == AURA_EVAL_OK, "float eval ok");
}

void test_state_persists(aura_eval_session* s) {
    std::printf("[AC3] state persists\n");
    int st = AURA_EVAL_INTERNAL;
    run(s, "(define aura-eval-counter 41)", &st);
    check(st == AURA_EVAL_OK, "define ok");
    const std::string rec = run(s, "(+ aura-eval-counter 1)", &st);
    check(st == AURA_EVAL_OK && has(rec.c_str(), "\"value\":\"42\""),
          "define is visible to the next call");
}

void test_moead(aura_eval_session* s) {
    std::printf("[AC4] require std/list + std/moead, ask\n");
    int st = AURA_EVAL_INTERNAL;
    run(s, "(require \"std/list\" all:)", &st);
    check(st == AURA_EVAL_OK, "require std/list");
    run(s, "(require \"std/math\" all:)", &st);
    check(st == AURA_EVAL_OK, "require std/math");
    run(s, "(require \"std/moead\" all:)", &st);
    check(st == AURA_EVAL_OK, "require std/moead");

    const std::string make_rec =
        run(s,
            "(define amr-evo (moead:make (hash \"n-obj\" 3 \"H\" 4 \"T\" 5 \"dim\" 4 "
            "\"seed\" 42 \"operator\" \"de\" "
            "\"objectives\" (list \"cost\" \"tokens\" \"sink_calls\") "
            "\"dirs\" (list \"min\" \"min\" \"min\"))))",
            &st);
    if (st != AURA_EVAL_OK)
        std::printf("    moead:make -> %s\n", make_rec.c_str());
    check(st == AURA_EVAL_OK, "moead:make");

    const std::string rec = run(s, "(length (moead:ask amr-evo 3))", &st);
    check(st == AURA_EVAL_OK && has(rec.c_str(), "\"value\":\"3\""),
          "moead:ask yields 3 candidates");

    const std::string tell =
        run(s,
            "(moead:tell! amr-evo (moead:ask amr-evo 1) "
            "(list (hash \"objectives\" (hash \"cost\" 1.0 \"tokens\" 2.0 \"sink_calls\" 3.0) "
            "\"constraints\" (hash \"quality\" 0.0))))",
            &st);
    check(st == AURA_EVAL_OK && has(tell.c_str(), "\"value\":\"#t\""), "moead:tell! returns #t");
}

void test_errors(aura_eval_session* s) {
    std::printf("[AC5] error paths\n");
    int st = AURA_EVAL_OK;
    const std::string rec = run(s, "(this-symbol-does-not-exist 1)", &st);
    check(st == AURA_EVAL_ERROR, "unbound name is AURA_EVAL_ERROR");
    check(has(rec.c_str(), "\"status\":\"error\""), "record says error");
    check(has(rec.c_str(), "\"message\""), "record carries a message");

    const std::string syntax = run(s, "(+ 1", &st);
    check(st == AURA_EVAL_ERROR && has(syntax.c_str(), "\"status\":\"error\""),
          "unbalanced form is an error record");

    // The session is still usable: an error is data, not a crash.
    const std::string after = run(s, "(* 6 7)", &st);
    check(st == AURA_EVAL_OK && has(after.c_str(), "\"value\":\"42\""),
          "session survives an error");

    const std::string refused = run(s, "(car 5)", &st);
    check(st == AURA_EVAL_ERROR && has(refused.c_str(), "error-value"),
          "error value is reported, not printed as a value");
}

void test_file(aura_eval_session* s) {
    std::printf("[AC6] file load\n");
    const char* path = "aura_eval_embed_test_source.aura";
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        check(false, "write test source");
        return;
    }
    const char* body =
        "(require \"std/list\" all:)\n(define from-file (length (list 1 2 3 4)))\nfrom-file\n";
    std::fwrite(body, 1, std::strlen(body), f);
    std::fclose(f);

    char* rec = nullptr;
    int rc = aura_eval_file(s, path, &rec);
    check(rc == AURA_EVAL_OK, "aura_eval_file ok");
    check(has(rec, "\"value\":\"4\""), "file result is 4");
    aura_eval_string_free(rec);
    std::remove(path);

    int st = AURA_EVAL_OK;
    char* missing = nullptr;
    rc = aura_eval_file(s, "no-such-file.aura", &missing);
    check(rc == AURA_EVAL_IO_ERROR, "missing file is AURA_EVAL_IO_ERROR");
    check(has(missing, "io-error"), "missing file record says io-error");
    aura_eval_string_free(missing);

    (void)st;
}

void test_invalid_arguments(aura_eval_session* s) {
    std::printf("[AC7] invalid arguments\n");
    char* rec = nullptr;
    check(aura_eval_source(nullptr, "(+ 1 1)", 5, &rec) == AURA_EVAL_INVALID_ARGUMENT,
          "null session refused");
    check(rec == nullptr, "no record on invalid argument");
    check(aura_eval_source(s, nullptr, 0, &rec) == AURA_EVAL_INVALID_ARGUMENT,
          "null source refused");
    check(aura_eval_source(s, "(+ 1 1)", 5, nullptr) == AURA_EVAL_INVALID_ARGUMENT,
          "null out-parameter refused");
    check(aura_eval_file(s, nullptr, &rec) == AURA_EVAL_INVALID_ARGUMENT, "null path refused");
}

void test_concurrent_callers(aura_eval_session* s) {
    std::printf("[AC8] concurrent callers on one session\n");
    std::atomic<int> ok{0};
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([s, &ok, &bad, t] {
            for (int i = 0; i < 10; ++i) {
                const std::string code =
                    "(* " + std::to_string(t + 1) + " " + std::to_string(i) + ")";
                int st = AURA_EVAL_INTERNAL;
                const std::string rec = run(s, code.c_str(), &st);
                if (st == AURA_EVAL_OK && has(rec.c_str(), "\"status\":\"ok\""))
                    ++ok;
                else
                    ++bad;
            }
        });
    }
    for (auto& th : threads)
        th.join();
    check(bad.load() == 0 && ok.load() == 40, "40 concurrent evals all ok");
}

} // namespace

int main() {
    // The test runs from the build tree; point the stdlib loader at the
    // source tree so (require "std/...") resolves without an install.
#ifdef AURA_SOURCE_DIR
    ::setenv("AURA_STDLIB_DIR", AURA_SOURCE_DIR "/lib/std", 0);
#endif
    ::setenv("AURA_PIPELINE_STRICT", "0", 0);
    // Do not force AURA_SANDBOX=off: the default Restricted posture plus the
    // Soft session grant must be enough for a stdlib that mutates its state.

    test_session_lifecycle();

    aura_eval_session* s = aura_eval_session_create("embed-test");
    if (!s) {
        std::printf("FAIL: cannot create session\n");
        return 1;
    }
    test_eval_string(s);
    test_state_persists(s);
    test_moead(s);
    test_errors(s);
    test_file(s);
    test_invalid_arguments(s);
    test_concurrent_callers(s);
    aura_eval_session_destroy(s);

    std::printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
