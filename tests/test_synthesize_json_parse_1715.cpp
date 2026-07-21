// @category: unit
// @reason: Issue #1715 — synthesize:define must parse LLM JSON via
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

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

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
    auto expr = std::string("(begin\n") + "  (define j (json-parse \"" + aura_string_escape(json) +
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

int main() {
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
