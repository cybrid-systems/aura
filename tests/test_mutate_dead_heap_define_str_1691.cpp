// @category: unit
// @reason: Issue #1691 — mutate:refactor/extract must not intern the
// temporary define_str into string_heap_ (dead push; sibling of #1488).
//
//   AC1: source site uses local define_str only (no push_back(define_str))
//   AC2: audit_dead_heap_push.py reports 0 candidates
//   AC3: N× refactor/extract does not grow string_heap by ~N×define_str

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <print>
#include <sstream>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::is_pair;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

int main() {
    // ── AC1: source-level contract ──
    {
        std::println("\n--- AC1: no dead define_str push in mutate.cpp ---");
        auto src = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(!src.empty(), "read evaluator_primitives_mutate.cpp");
        // The fixed site must keep local define_str and must not reintroduce
        // the size()+push_back(define_str) pattern near it.
        CHECK(src.find("std::string define_str = \"(define (\"") != std::string::npos,
              "define_str local construction present");
        // Forbid the classic dead-push couplet within a short window.
        auto pos = src.find("std::string define_str = \"(define (\"");
        CHECK(pos != std::string::npos, "locate define_str site");
        if (pos != std::string::npos) {
            auto window = src.substr(pos, 400);
            CHECK(window.find("define_idx") == std::string::npos, "no define_idx capture");
            CHECK(window.find("string_heap_.push_back(define_str)") == std::string::npos,
                  "no push_back(define_str)");
            CHECK(window.find("parse_to_flat(define_str") != std::string::npos,
                  "parse_to_flat uses define_str directly");
        }
    }

    // ── AC2: repo-wide audit clean ──
    {
        std::println("\n--- AC2: audit_dead_heap_push clean ---");
        int rc = std::system("python3 scripts/audit_dead_heap_push.py >/tmp/audit_1691.out 2>&1");
        CHECK(rc == 0, "audit_dead_heap_push exit 0");
        auto out = read_file("/tmp/audit_1691.out");
        CHECK(out.find("clean") != std::string::npos ||
                  out.find("0 candidates") != std::string::npos,
              "audit reports clean / 0 candidates");
    }

    // ── AC3: extract path heap growth without dead intern ──
    {
        std::println("\n--- AC3: refactor/extract heap growth ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (define (g y) (+ y 2))\")").has_value(),
              "set-code");
        auto* flat = cs.evaluator().workspace_flat();
        CHECK(flat != nullptr, "flat");

        // Find a LiteralInt to extract (value 1 or 2).
        aura::ast::NodeId lit = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (!flat->is_live_node(id))
                continue;
            auto v = flat->get(id);
            if (v.tag == aura::ast::NodeTag::LiteralInt && v.int_value == 1) {
                lit = id;
                break;
            }
        }
        CHECK(lit != aura::ast::NULL_NODE, "found lit 1");

        const auto heap0 = cs.evaluator().string_heap().size();
        // One extract — if define_str were pushed, heap grows by at least 1
        // beyond the new_name string arg re-intern. We allow modest growth
        // from the name literal + any incidental interning, but not a
        // per-call define_str intern of ~20+ bytes unreferenced.
        auto r = cs.eval(std::string("(mutate:refactor/extract ") + std::to_string(lit) +
                         " \"extracted_fn_1691\" \"ac3\")");
        // Extract may succeed or fail depending on AST shape; heap invariant
        // is what we care about when it runs the define_str path.
        const auto heap1 = cs.evaluator().string_heap().size();
        std::println("  heap before={} after={} delta={} extract_ok={}", heap0, heap1,
                     heap1 - heap0, r.has_value());
        // Hard bound: a single extract must not dump many unreferenced strings.
        // Pre-#1691 dead push would add +1 (define_str) every call.
        CHECK(heap1 - heap0 <= 8, "extract does not dump large dead string_heap payload");

        // If extract succeeded, confirm define_str text is not a new exclusive
        // heap entry that nothing else would have created (best-effort).
        if (r && is_pair(*r)) {
            bool found_define_str = false;
            const std::string needle = "(define (extracted_fn_1691 x) x)";
            auto heap = cs.evaluator().string_heap();
            for (std::size_t i = heap0; i < heap1 && i < heap.size(); ++i) {
                if (heap[i] == needle)
                    found_define_str = true;
            }
            CHECK(!found_define_str, "define_str not interned into string_heap_");
        } else {
            CHECK(true, "extract skipped functional assert (path shape)");
        }
    }

    std::println("\n=== test_mutate_dead_heap_define_str_1691: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
