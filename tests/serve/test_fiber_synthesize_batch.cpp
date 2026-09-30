// test_fiber_synthesize_batch.cpp — consolidated fiber-theme drivers
// Merged from per-issue standalones; each section in its own namespace.
// Prefer adding a section here over a new tests/fiber binary.

#include "test_harness.hpp"
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;


// ─── from test_synthesize_json_parse.cpp →
// aura_fiber_run_synthesize_json_1715::run_synthesize_json_1715 ───
namespace aura_fiber_run_synthesize_json_1715 {
// @category: unit
// @reason: Issue #1715 — synthesize:define must parse LLM JSON via
// Issue #1715 (#1978 renamed): issue# moved from filename to header.
// json-parse structure walk (not hand-rolled find("content") scan).
//
//   AC1: source uses json-parse / hash-ref; no find("content") scanner
//   AC2: json-parse pretty-printed choices[0].message.content
//   AC3: json-parse embedded escaped quotes in content
//   AC4: json-parse \uXXXX unicode (λ = U+03BB)
//   AC5: null content → non-string (void)

// Compatibility note: parse_keyword lambda in
// src/compiler/evaluator_primitives_json.cpp takes std::string_view
// (migrated from const std::string& per the core string_view pass).
// This test exercises the JSON parsing path that calls parse_keyword;
// the type change is behavior-neutral because string_view auto-binds
// from std::string/std::string literal/const char* and parse_keyword
// only reads kw.size() + substr() + compare — all string_view ops.


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_string_idx;
    using aura::compiler::types::is_string;
    using aura::compiler::types::is_void;
    using aura::test::g_failed;
    using aura::test::g_passed;

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::string heap_str(CompilerService& cs, const aura::compiler::types::EvalValue& v) {
        if (!is_string(v))
            return {};
        auto& heap = cs.evaluator().string_heap_mut();
        auto idx = as_string_idx(v);
        if (idx >= heap.size())
            return {};
        return heap[idx];
    }

    // Escape so `json` can sit inside an Aura double-quoted string literal.
    std::string aura_string_escape(std::string_view s) {
        std::string o;
        o.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '\\':
                case '"':
                    o += '\\';
                    o += c;
                    break;
                case '\n':
                    o += "\\n";
                    break;
                case '\r':
                    o += "\\r";
                    break;
                case '\t':
                    o += "\\t";
                    break;
                default:
                    o += c;
                    break;
            }
        }
        return o;
    }

    // Same structure walk as synthesize:define (#1715):
    // choices → car → message → content via json-parse + hash-ref.
    std::string extract_content(CompilerService& cs, std::string_view json) {
        auto expr =
            std::string("(begin\n") + "  (define j (json-parse \"" + aura_string_escape(json) +
            "\"))\n" + "  (define ch (hash-ref j \"choices\"))\n" + "  (define c0 (car ch))\n" +
            "  (define msg (hash-ref c0 \"message\"))\n" + "  (hash-ref msg \"content\"))\n";
        auto step = cs.eval(expr);
        if (!step)
            return {};
        if (is_void(*step))
            return {};
        return heap_str(cs, *step);
    }

} // namespace

int run_synthesize_json_1715() {
    // ── AC1: source audit ──
    {
        std::println("\n--- AC1: source uses json-parse structure walk ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_agent.cpp",
            "../src/compiler/evaluator_primitives_agent.cpp",
        };
        std::string src;
        for (const char* p : candidates) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read agent primitives");
        if (!src.empty()) {
            CHECK(src.find("Issue #1715") != std::string::npos, "cites #1715");
            auto pos = src.find("add(\"synthesize:define\"");
            CHECK(pos != std::string::npos, "found synthesize:define");
            if (pos != std::string::npos) {
                auto end = src.find("\n    add(\"", pos + 10);
                auto win = src.substr(pos, end == std::string::npos ? 8000 : end - pos);
                CHECK(win.find("json-parse") != std::string::npos, "uses json-parse");
                CHECK(win.find("hash-ref") != std::string::npos, "uses hash-ref");
                CHECK(win.find("kMaxSynthCodeBytes") != std::string::npos, "size cap");
                CHECK(win.find("response.find(\"content\")") == std::string::npos &&
                          win.find("find(\"content\")") == std::string::npos,
                      "no find(\"content\") scanner");
            }
        }
        std::string jsrc;
        for (const char* p : {"src/compiler/evaluator_primitives_json.cpp",
                              "../src/compiler/evaluator_primitives_json.cpp"}) {
            jsrc = read_file(p);
            if (!jsrc.empty())
                break;
        }
        CHECK(!jsrc.empty() && jsrc.find("Issue #1715") != std::string::npos,
              "json-parse cites #1715 unicode");
        CHECK(jsrc.find("case 'u'") != std::string::npos, "\\uXXXX case present");
    }

    // ── AC2: pretty-printed JSON ──
    {
        std::println("\n--- AC2: pretty-printed content ---");
        CompilerService cs;
        // Use JSON delimiter so )" inside content does not end the raw string.
        const char* json = R"JSON({
  "choices": [
    {
      "message": {
        "role": "assistant",
        "content": "(define (f x) x)"
      }
    }
  ]
})JSON";
        auto code = extract_content(cs, json);
        CHECK(code.find("define") != std::string::npos, "got define body");
        CHECK(code.find("f x") != std::string::npos || code.find("(f x)") != std::string::npos,
              "got function shape");
    }

    // ── AC3: escaped quote inside JSON string value ──
    {
        std::println("\n--- AC3: json-parse decodes escaped quotes ---");
        CompilerService cs;
        // Build JSON on the heap via C++ to avoid Aura nested-escape pitfalls.
        auto& heap = cs.evaluator().string_heap_mut();
        auto jidx = heap.size();
        heap.push_back(R"JSON({"choices":[{"message":{"content":"(define s \"hi\")"}}]})JSON");
        auto parse_fn = cs.evaluator().primitives().lookup("json-parse");
        auto href_fn = cs.evaluator().primitives().lookup("hash-ref");
        CHECK(parse_fn && href_fn, "json-parse and hash-ref present");
        if (parse_fn && href_fn) {
            using aura::compiler::types::as_pair_idx;
            using aura::compiler::types::is_hash;
            using aura::compiler::types::is_pair;
            using aura::compiler::types::make_string;
            auto root = (*parse_fn)({make_string(jidx)});
            CHECK(is_hash(root), "root is hash");
            auto push_key = [&](const char* k) {
                auto i = heap.size();
                heap.push_back(k);
                return make_string(i);
            };
            auto choices = (*href_fn)({root, push_key("choices")});
            CHECK(is_pair(choices), "choices is list");
            if (is_pair(choices)) {
                auto c0 = cs.evaluator().pairs()[as_pair_idx(choices)].car;
                auto msg = (*href_fn)({c0, push_key("message")});
                auto content = (*href_fn)({msg, push_key("content")});
                auto code = heap_str(cs, content);
                CHECK(code.find("define") != std::string::npos, "define present");
                CHECK(code.find("hi") != std::string::npos, "literal hi present");
                CHECK(code.find('"') != std::string::npos, "quotes decoded into code");
            }
        }
    }

    // ── AC4: unicode \u03bb (λ) ──
    {
        std::println("\n--- AC4: unicode escape \\u03bb ---");
        CompilerService cs;
        // Push JSON string "\u03bb" via C++ heap (avoid Aura escape maze).
        auto& heap = cs.evaluator().string_heap_mut();
        auto jidx = heap.size();
        heap.push_back("\"\\u03bb\""); // JSON text for string value λ
        auto parse_fn = cs.evaluator().primitives().lookup("json-parse");
        CHECK(parse_fn, "json-parse present");
        if (parse_fn) {
            using aura::compiler::types::make_string;
            auto r = (*parse_fn)({make_string(jidx)});
            CHECK(is_string(r), "parse unicode string");
            if (is_string(r)) {
                auto s = heap_str(cs, r);
                CHECK(s.size() == 2 && static_cast<unsigned char>(s[0]) == 0xCE &&
                          static_cast<unsigned char>(s[1]) == 0xBB,
                      "\\u03bb → UTF-8 λ");
            }
        }
    }

    // ── AC5: null content ──
    {
        std::println("\n--- AC5: null content ---");
        CompilerService cs;
        const char* json = R"JSON({"choices":[{"message":{"content":null}}]})JSON";
        auto code = extract_content(cs, json);
        CHECK(code.empty(), "null content → empty (not garbage)");
    }

    std::println("\n=== test_synthesize_json_parse_1715: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_synthesize_json_1715
// ─── end test_synthesize_json_parse.cpp ───

// ─── from test_synthesize_optimize_prng.cpp →
// aura_fiber_run_synthesize_prng_1716::run_synthesize_prng_1716 ───
namespace aura_fiber_run_synthesize_prng_1716 {
// @category: unit
// @reason: Issue #1716 — synthesize:optimize must not use std::rand()
// Issue #1716 (#1978 renamed): issue# moved from filename to header.
// (non-thread-safe); use thread_local mt19937 instead.
//
//   AC1: source has no std::rand / RAND_MAX in synthesize:optimize
//   AC2: source uses thread_local mt19937 / agent_prng
//   AC3: cites Issue #1716
//   AC4: synthesize:optimize primitive still registered (callable)


namespace {

    using aura::compiler::CompilerService;
    using aura::test::g_failed;
    using aura::test::g_passed;

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // Live code only: strip // line comments before scanning for banned APIs.
    std::string strip_line_comments(std::string_view win) {
        std::string code;
        code.reserve(win.size());
        for (size_t i = 0; i < win.size();) {
            if (i + 1 < win.size() && win[i] == '/' && win[i + 1] == '/') {
                while (i < win.size() && win[i] != '\n')
                    ++i;
                continue;
            }
            code.push_back(win[i++]);
        }
        return code;
    }

} // namespace

int run_synthesize_prng_1716() {
    // ── AC1/AC2/AC3: source audit ──
    {
        std::println("\n--- AC1/AC2/AC3: thread_local PRNG, no std::rand ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_agent.cpp",
            "../src/compiler/evaluator_primitives_agent.cpp",
        };
        std::string src;
        for (const char* p : candidates) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read agent primitives");
        if (!src.empty()) {
            CHECK(src.find("Issue #1716") != std::string::npos, "cites #1716");
            CHECK(src.find("thread_local std::mt19937") != std::string::npos,
                  "thread_local mt19937");
            CHECK(src.find("agent_prng") != std::string::npos, "agent_prng helper");
            CHECK(src.find("agent_rand_below") != std::string::npos, "agent_rand_below");
            CHECK(src.find("agent_rand_unit") != std::string::npos, "agent_rand_unit");

            auto pos = src.find("add(\"synthesize:optimize\"");
            CHECK(pos != std::string::npos, "found synthesize:optimize");
            if (pos != std::string::npos) {
                // Body until next top-level add(
                auto end = src.find("\n    add(\"", pos + 10);
                auto win = src.substr(pos, end == std::string::npos ? 12000 : end - pos);
                auto code = strip_line_comments(win);
                CHECK(code.find("std::rand") == std::string::npos, "no live std::rand");
                CHECK(code.find("RAND_MAX") == std::string::npos, "no live RAND_MAX");
                CHECK(code.find("agent_rand") != std::string::npos,
                      "optimize body uses agent_rand*");
            }
        }
    }

    // ── AC4: primitive exists ──
    {
        std::println("\n--- AC4: synthesize:optimize registered ---");
        CompilerService cs;
        auto r = cs.eval("(procedure? synthesize:optimize)");
        CHECK(r.has_value(), "eval ok");
        if (r) {
            using aura::compiler::types::as_bool;
            using aura::compiler::types::is_bool;
            CHECK(is_bool(*r) && as_bool(*r), "synthesize:optimize is a procedure");
        }
    }

    std::println("\n=== test_synthesize_optimize_prng_1716: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_synthesize_prng_1716
// ─── end test_synthesize_optimize_prng.cpp ───

// ─── from test_try_probe_heap_slot.cpp → aura_fiber_run_try_probe_1718::run_try_probe_1718 ───
namespace aura_fiber_run_try_probe_1718 {
// @category: unit
// @reason: Issue #1718 — synthesize:optimize try_probe must reuse a fixed
// Issue #1718 (#1978 renamed): issue# moved from filename to header.
// string_heap slot, not push_back every probe (unbounded heap growth).
//
//   AC1: source cites #1718 and uses probe_slot reuse
//   AC2: try_probe assigns string_heap_[probe_slot] (not only push_back)
//   AC3: no unbounded push_back(call_src) without slot reuse in fitness
//   AC4: synthesize:optimize still registered


namespace {

    using aura::compiler::CompilerService;
    using aura::test::g_failed;
    using aura::test::g_passed;

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::string strip_line_comments(std::string_view win) {
        std::string code;
        code.reserve(win.size());
        for (size_t i = 0; i < win.size();) {
            if (i + 1 < win.size() && win[i] == '/' && win[i + 1] == '/') {
                while (i < win.size() && win[i] != '\n')
                    ++i;
                continue;
            }
            code.push_back(win[i++]);
        }
        return code;
    }

} // namespace

int run_try_probe_1718() {
    // ── AC1–AC3: source audit ──
    {
        std::println("\n--- AC1–AC3: probe_slot reuse ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_agent.cpp",
            "../src/compiler/evaluator_primitives_agent.cpp",
        };
        std::string src;
        for (const char* p : candidates) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read agent primitives");
        if (!src.empty()) {
            CHECK(src.find("Issue #1718") != std::string::npos, "cites #1718");
            CHECK(src.find("probe_slot") != std::string::npos, "probe_slot present");

            auto pos = src.find("add(\"synthesize:optimize\"");
            CHECK(pos != std::string::npos, "found synthesize:optimize");
            if (pos != std::string::npos) {
                auto end = src.find("\n    add(\"", pos + 10);
                auto win = src.substr(pos, end == std::string::npos ? 25000 : end - pos);
                auto code = strip_line_comments(win);
                // try_probe region
                auto tp = code.find("try_probe");
                CHECK(tp != std::string::npos, "try_probe present");
                if (tp != std::string::npos) {
                    auto region = code.substr(tp, 900);
                    CHECK(region.find("probe_slot") != std::string::npos,
                          "try_probe uses probe_slot");
                    CHECK(region.find("push_back(call_src)") != std::string::npos,
                          "first-time push still present");
                    CHECK(region.find("string_heap_[probe_slot]") != std::string::npos,
                          "in-place assign to slot");
                    CHECK(region.find("make_string(probe_slot)") != std::string::npos,
                          "eval uses probe_slot");
                }
            }
        }
    }

    // ── AC4: primitive registered ──
    {
        std::println("\n--- AC4: synthesize:optimize registered ---");
        CompilerService cs;
        auto r = cs.eval("(procedure? synthesize:optimize)");
        CHECK(r.has_value(), "eval ok");
        if (r) {
            using aura::compiler::types::as_bool;
            using aura::compiler::types::is_bool;
            CHECK(is_bool(*r) && as_bool(*r), "synthesize:optimize is a procedure");
        }
    }

    std::println("\n=== test_try_probe_heap_slot_1718: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_try_probe_1718
// ─── end test_try_probe_heap_slot.cpp ───

// ─── from test_find_after_parens.cpp → aura_fiber_run_find_after_parens::run_find_after_parens ───
namespace aura_fiber_run_find_after_parens {
// @category: unit
// @reason: Issue #1723 — evolve-strategy analytics find_after must be
// Issue #1723 (#1978 renamed): issue# moved from filename to header.
// paren-aware (nested values / top-errors lists).
//
//   AC1: source cites #1723; find_after tracks depth
//   AC2: top-errors matcher uses paren depth (not first ')')
//   AC3: evolve-strategy with nested top-errors + numeric rates still evolves
//   AC4: plain numeric analytics still parse (success-rate / avg-attempts)


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_int;
    using aura::test::g_failed;
    using aura::test::g_passed;

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // Body of add("evolve-strategy" ... until next top-level add(
    std::string evolve_window(const std::string& src) {
        auto needle = std::string("add(\"evolve-strategy\"");
        auto pos = src.find(needle);
        if (pos == std::string::npos)
            return {};
        auto end = src.find("\n    add(\"", pos + 10);
        return src.substr(pos, end == std::string::npos ? 8000 : end - pos);
    }

} // namespace

int run_find_after_parens() {
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: paren-aware find_after / top-errors ---");
        std::string agent;
        for (const char* p : {"src/compiler/evaluator_primitives_agent.cpp",
                              "../src/compiler/evaluator_primitives_agent.cpp"}) {
            agent = read_file(p);
            if (!agent.empty())
                break;
        }
        CHECK(!agent.empty(), "read agent");
        CHECK(agent.find("#1723") != std::string::npos, "cites #1723");

        auto win = evolve_window(agent);
        CHECK(!win.empty(), "found evolve-strategy");
        CHECK(win.find("find_after") != std::string::npos, "has find_after");
        CHECK(win.find("depth") != std::string::npos, "tracks paren depth");
        // Must not keep the naive end condition as the only exit path.
        CHECK(win.find("te_depth") != std::string::npos ||
                  win.find("top-errors") != std::string::npos,
              "top-errors path present");
        CHECK(win.find("te_depth") != std::string::npos, "top-errors uses te_depth");
    }

    // ── AC3/AC4: evolve with nested top-errors + numeric rates ──
    {
        std::println("\n--- AC3/AC4: evolve-strategy analytics parse ---");
        CompilerService cs;

        auto def = cs.eval(R"AURA((define-strategy "s1723" "(lambda (x) x)" :max-attempts 3))AURA");
        CHECK(static_cast<bool>(def), "define-strategy s1723");

        // Nested list inside top-errors; rates are plain atoms.
        // success-rate 0.2 + avg-attempts 3 with max-attempts 3 → bump +2.
        auto evo = cs.eval(
            R"AURA((evolve-strategy "s1723" "#(analytics total-runs:10 success-rate:0.2 avg-attempts:3 total-llm-calls:1 avg-duration-ms:1 top-errors:( unbound-variable:3 (nested-key:2) type-error:1) by-task:((nested task name) 1/2))"))AURA");
        CHECK(static_cast<bool>(evo), "evolve-strategy with nested analytics");

        // Field max-attempts should be 5 after bump (3+2) if rates parsed.
        auto field = cs.eval(R"AURA((strategy-field "s1723-v1" "max-attempts"))AURA");
        CHECK(field.has_value() && is_int(*field), "strategy-field max-attempts is int");
        if (field && is_int(*field))
            CHECK(as_int(*field) == 5, "low success-rate → max-attempts bumped to 5");

        // High success path still parses (lower max-attempts).
        auto def2 =
            cs.eval(R"AURA((define-strategy "s1723hi" "(lambda (x) x)" :max-attempts 4))AURA");
        CHECK(static_cast<bool>(def2), "define-strategy s1723hi");
        auto evo2 = cs.eval(
            R"AURA((evolve-strategy "s1723hi" "#(analytics total-runs:10 success-rate:0.95 avg-attempts:1.0 total-llm-calls:1 avg-duration-ms:1 top-errors:() by-task:())"))AURA");
        CHECK(static_cast<bool>(evo2), "evolve high-success analytics");
        auto field2 = cs.eval(R"AURA((strategy-field "s1723hi-v1" "max-attempts"))AURA");
        CHECK(field2.has_value() && is_int(*field2), "high-success max-attempts is int");
        if (field2 && is_int(*field2))
            CHECK(as_int(*field2) == 3, "high success-rate → max-attempts lowered to 3");
    }

    std::println("\n=== test_find_after_parens_1723: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_find_after_parens
// ─── end test_find_after_parens.cpp ───

// ─── Issue #4267 — synthesize:optimize score synthesis ───
namespace aura_fiber_run_optimize_score_4267 {
// @category: unit
// @reason: Issue #4267 — synthesize:optimize score synthesis: (1) the
// default score was the probe non-error rate plus a shorter-source bonus
// (labeled "correctness") — a shorter-wrong variant could beat the baseline;
// (2) the user's :fitness / :benchmark expression was read but never eval'd
// (the candidate source was eval'd instead — a define form, non-numeric, so
// every candidate scored 0); (3) option keys arrive keyword-tagged (Issue
// #63 Phase 3) so the string-only parse silently dropped every :keyword
// option. Fix: resolve keyword keys, eval the :fitness expression as the
// score (non-numeric = candidate rejected), default path scores by
// agreement with baseline probe results (semantic-preserving) with length
// as a pure post-gate tiebreaker.
//
//   AC1: :fitness expression is the eval path — opposite fitness exprs
//        ((f 3) vs (- 0 (f 3))) produce expression-derived, non-default
//        scores that flip the ranking (A ≥ 13, B ≥ -13, A - B ≥ 26)
//   AC2: :benchmark alias behaves like :fitness (expression-derived score)
//   AC3: non-numeric fitness rejects the candidate — baseline retained
//        (gen 0) with the reject sentinel score (< -1e300)
//   AC4: default score is baseline-agreement — a default run never returns
//        a sub-1000 score (a shorter-wrong variant cannot win)
//   AC5: source contract — keyword-key resolution in the option loop,
//        reference-agreement scoring, no `correctness` identifier in the
//        comment-stripped optimize region, #4267 cited

namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_pair_idx;
    using aura::compiler::types::as_string_idx;
    using aura::compiler::types::is_pair;
    using aura::compiler::types::is_string;
    using aura::test::g_failed;
    using aura::test::g_passed;

    std::string read_file(const char* path) {
        std::ifstream in(path);
        if (!in)
            return {};
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    std::string strip_line_comments(std::string_view win) {
        std::string code;
        code.reserve(win.size());
        for (size_t i = 0; i < win.size();) {
            if (i + 1 < win.size() && win[i] == '/' && win[i + 1] == '/') {
                while (i < win.size() && win[i] != '\n')
                    ++i;
                continue;
            }
            code.push_back(win[i++]);
        }
        return code;
    }

    // synthesize:optimize returns (gen . fitness) as a pair of heap strings.
    double returned_fitness(CompilerService& cs, const aura::compiler::types::EvalValue& v) {
        if (!is_pair(v))
            return 0.0;
        auto pi = as_pair_idx(v);
        auto& pairs = cs.evaluator().pairs();
        if (pi >= pairs.size())
            return 0.0;
        const auto& fit = pairs[pi].cdr;
        if (!is_string(fit))
            return 0.0;
        auto si = as_string_idx(fit);
        auto& heap = cs.evaluator().string_heap_mut();
        if (si >= heap.size())
            return 0.0;
        try {
            return std::stod(heap[si]);
        } catch (...) {
            return 0.0;
        }
    }

    std::int64_t returned_gen(CompilerService& cs, const aura::compiler::types::EvalValue& v) {
        if (!is_pair(v))
            return -1;
        auto pi = as_pair_idx(v);
        auto& pairs = cs.evaluator().pairs();
        if (pi >= pairs.size())
            return -1;
        const auto& gen = pairs[pi].car;
        if (!is_string(gen))
            return -1;
        auto si = as_string_idx(gen);
        auto& heap = cs.evaluator().string_heap_mut();
        if (si >= heap.size())
            return -1;
        try {
            return std::stoll(heap[si]);
        } catch (...) {
            return -1;
        }
    }

    bool install_baseline(CompilerService& cs) {
        auto sc = cs.eval("(set-code \"(define (f x) (+ x 10))\")");
        if (!sc)
            return false;
        auto tc = cs.eval("(typecheck-current)");
        return tc.has_value();
    }

} // namespace

int run_optimize_score_4267() {
    // ── AC1: :fitness expression is the eval path (ranking flip) ──
    double fitness_a = 0.0;
    double fitness_b = 0.0;
    {
        std::println("\n--- AC1: :fitness expression on the eval path ---");
        CompilerService cs;
        CHECK(install_baseline(cs), "baseline (+ x 10) installed");
        auto ra = cs.eval("(synthesize:optimize \"f\" :fitness \"(f 3)\")");
        CHECK(ra.has_value(), "fitness-A eval ok");
        if (ra)
            fitness_a = returned_fitness(cs, *ra);
        auto rb = cs.eval("(synthesize:optimize \"f\" :fitness \"(- 0 (f 3))\")");
        CHECK(rb.has_value(), "fitness-B eval ok");
        if (rb)
            fitness_b = returned_fitness(cs, *rb);
        // Baseline (f 3) = 13 is always in the population, so A ≥ 13 and
        // B ≥ -13; expression-derived scores stay far below the ~1000
        // default-scorer signature. At HEAD both returned 1000.03125 (the
        // default non-error rate) — the expression never ran.
        CHECK(fitness_a >= 13.0, "fitness-A derives from (f 3) — ≥ baseline 13");
        CHECK(fitness_b >= -13.0, "fitness-B derives from (- 0 (f 3)) — ≥ baseline -13");
        CHECK(fitness_a < 1000.0 && fitness_b < 1000.0,
              "scores are expression values, not the default non-error rate");
        CHECK(fitness_a - fitness_b >= 26.0,
              "opposite :fitness expressions flip the ranking (A - B ≥ 26)");
    }

    // ── AC2: :benchmark alias ──
    {
        std::println("\n--- AC2: :benchmark alias honors the expression ---");
        CompilerService cs;
        CHECK(install_baseline(cs), "baseline installed");
        auto r = cs.eval("(synthesize:optimize \"f\" :benchmark \"(f 3)\")");
        CHECK(r.has_value(), "benchmark eval ok");
        double fb = r ? returned_fitness(cs, *r) : 0.0;
        CHECK(fb >= 13.0 && fb < 1000.0, ":benchmark score is expression-derived (≥ 13, < 1000)");
    }

    // ── AC3: non-numeric fitness rejects the candidate ──
    {
        std::println("\n--- AC3: non-numeric fitness rejects candidate ---");
        CompilerService cs;
        CHECK(install_baseline(cs), "baseline installed");
        auto r = cs.eval("(synthesize:optimize \"f\" :fitness \"(no-such-fn 3)\")");
        CHECK(r.has_value(), "eval ok");
        if (r) {
            auto gen = returned_gen(cs, *r);
            double fit = returned_fitness(cs, *r);
            CHECK(gen == 0, "no candidate beats a rejected-fitness baseline (gen 0)");
            CHECK(fit < -1e300, "non-numeric fitness → reject sentinel score");
        }
    }

    // ── AC4: default score is baseline-agreement ──
    {
        std::println("\n--- AC4: default score is baseline-agreement ---");
        CompilerService cs;
        CHECK(install_baseline(cs), "baseline installed");
        auto r = cs.eval("(synthesize:optimize \"f\")");
        CHECK(r.has_value(), "default optimize eval ok");
        if (r) {
            double fit = returned_fitness(cs, *r);
            // Baseline self-agreement is a perfect 1000; a shorter-wrong
            // variant (e.g. (+ x 0)) diverges on every probe and cannot win.
            CHECK(fit >= 1000.0, "default run keeps baseline agreement (≥ 1000)");
        }
    }

    // ── AC5: source contract ──
    {
        std::println("\n--- AC5: source contract in evaluator_primitives_agent.cpp ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_agent.cpp",
            "../src/compiler/evaluator_primitives_agent.cpp",
        };
        std::string src;
        for (const char* p : candidates) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read agent primitives");
        if (!src.empty()) {
            auto pos = src.find("add(\"synthesize:optimize\"");
            CHECK(pos != std::string::npos, "found synthesize:optimize");
            if (pos != std::string::npos) {
                auto end = src.find("\n    add(\"", pos + 10);
                auto win = src.substr(pos, end == std::string::npos ? 30000 : end - pos);
                auto code = strip_line_comments(win);
                CHECK(win.find("Issue #4267") != std::string::npos, "region cites #4267");
                CHECK(code.find("types::as_keyword_idx") != std::string::npos,
                      "option loop resolves keyword-tagged keys");
                CHECK(code.find("ref_results") != std::string::npos &&
                          code.find("ref_ready") != std::string::npos,
                      "default path records baseline reference results");
                CHECK(code.find("agreement") != std::string::npos,
                      "default score is baseline-agreement");
                CHECK(code.find("std::numeric_limits<double>::lowest()") != std::string::npos,
                      "non-numeric fitness rejects the candidate");
                CHECK(code.find("push_string_heap(fitness_expr)") != std::string::npos,
                      ":fitness expression is what gets eval'd");
                CHECK(code.find("push_string_heap(src)") == std::string::npos,
                      "candidate source is no longer eval'd as the fitness");
                CHECK(code.find("correctness") == std::string::npos,
                      "no `correctness` identifier in the optimize region");
            }
        }
    }

    std::println("\n=== run_optimize_score_4267: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_optimize_score_4267
// ─── end Issue #4267 ───

int main() {
    std::println("\n######## run_synthesize_json_1715 ########");
    if (int rc = aura_fiber_run_synthesize_json_1715::run_synthesize_json_1715(); rc != 0) {
        std::println("run_synthesize_json_1715 FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_synthesize_prng_1716 ########");
    if (int rc = aura_fiber_run_synthesize_prng_1716::run_synthesize_prng_1716(); rc != 0) {
        std::println("run_synthesize_prng_1716 FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_try_probe_1718 ########");
    if (int rc = aura_fiber_run_try_probe_1718::run_try_probe_1718(); rc != 0) {
        std::println("run_try_probe_1718 FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_find_after_parens ########");
    if (int rc = aura_fiber_run_find_after_parens::run_find_after_parens(); rc != 0) {
        std::println("run_find_after_parens FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_optimize_score_4267 ########");
    if (int rc = aura_fiber_run_optimize_score_4267::run_optimize_score_4267(); rc != 0) {
        std::println("run_optimize_score_4267 FAILED rc={}", rc);
        return rc;
    }
    if (::aura::test::g_failed)
        return 1;
    std::println("\ntest_fiber_synthesize_batch: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
