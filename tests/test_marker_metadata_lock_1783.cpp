// @category: unit
// @reason: Issue #1783 — syntax:set-marker / set-provenance must lock
// marker_ / provenance_ columns (FlatAST metadata_mtx_) so concurrent
// fibers cannot race on metadata-only writes.
//
//   AC1: source has metadata_mtx_ + begin_metadata_mutation / reader lock
//   AC2: set-marker / syntax-marker use metadata locks
//   AC3: set-provenance / get-provenance use metadata locks
//   AC4: set/get marker and provenance round-trip under CompilerService
//   AC5: multi-thread FlatAST metadata write/read stress terminates

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::SyntaxMarker;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;
using clock = std::chrono::steady_clock;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

} // namespace

int main() {
    // ── AC1: FlatAST plumbing ──
    {
        std::println("\n--- AC1: FlatAST metadata_mtx_ guards ---");
        auto ast = read_first({"src/core/ast.ixx", "../src/core/ast.ixx"});
        CHECK(!ast.empty(), "read ast.ixx");
        CHECK(ast.find("#1783") != std::string::npos, "cites #1783");
        CHECK(ast.find("metadata_mtx_") != std::string::npos, "has metadata_mtx_");
        CHECK(ast.find("begin_metadata_mutation") != std::string::npos,
              "has begin_metadata_mutation");
        CHECK(ast.find("try_acquire_metadata_reader_lock") != std::string::npos,
              "has metadata reader lock");
        CHECK(ast.find("class MetadataWriteGuard") != std::string::npos, "MetadataWriteGuard");
        CHECK(ast.find("class MetadataReadGuard") != std::string::npos, "MetadataReadGuard");
    }

    // ── AC2/AC3: primitive sources ──
    {
        std::println("\n--- AC2/AC3: primitives lock metadata ---");
        auto prim = read_first({"src/compiler/evaluator_primitives_compile_05.cpp",
                                "../src/compiler/evaluator_primitives_compile_05.cpp"});
        CHECK(!prim.empty(), "read compile_05.cpp");
        CHECK(prim.find("#1783") != std::string::npos, "cites #1783");

        auto set_m = prim.find("add(\"syntax:set-marker\"");
        CHECK(set_m != std::string::npos, "set-marker present");
        auto win_sm = prim.substr(set_m, 2500);
        CHECK(win_sm.find("begin_metadata_mutation") != std::string::npos,
              "set-marker takes write lock");

        auto get_m = prim.find("add(\"syntax-marker\"");
        CHECK(get_m != std::string::npos, "syntax-marker present");
        auto win_gm = prim.substr(get_m, 800);
        CHECK(win_gm.find("try_acquire_metadata_reader_lock") != std::string::npos,
              "syntax-marker takes read lock");

        auto set_p = prim.find("add(\"syntax:set-provenance\"");
        CHECK(set_p != std::string::npos, "set-provenance present");
        auto win_sp = prim.substr(set_p, 1200);
        CHECK(win_sp.find("begin_metadata_mutation") != std::string::npos,
              "set-provenance takes write lock");

        auto get_p = prim.find("add(\"syntax:get-provenance\"");
        CHECK(get_p != std::string::npos, "get-provenance present");
        auto win_gp = prim.substr(get_p, 900);
        CHECK(win_gp.find("try_acquire_metadata_reader_lock") != std::string::npos,
              "get-provenance takes read lock");

        auto prop = prim.find("add(\"syntax:propagate-marker\"");
        CHECK(prop != std::string::npos, "propagate-marker present");
        auto win_pr = prim.substr(prop, 1200);
        CHECK(win_pr.find("begin_metadata_mutation") != std::string::npos,
              "propagate-marker holds write lock");
    }

    // ── AC4: functional round-trip ──
    {
        std::println("\n--- AC4: marker + provenance round-trip ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define z 1)\")").has_value(), "set-code");
        auto r = cs.eval("(car (query:find \"z\"))");
        CHECK(r && is_int(*r), "find z");
        const auto id = as_int(*r);

        auto set = cs.eval(std::format("(syntax:set-marker {} 1)", id));
        CHECK(set && is_bool(*set) && as_bool(*set), "set-marker ok");
        auto m = cs.eval(std::format("(syntax-marker {})", id));
        CHECK(m && is_int(*m) && as_int(*m) == 1, "syntax-marker reads 1");

        auto sp = cs.eval(std::format("(syntax:set-provenance {} 42)", id));
        CHECK(sp && is_bool(*sp) && as_bool(*sp), "set-provenance ok");
        auto gp = cs.eval(std::format("(syntax:get-provenance {})", id));
        CHECK(gp && is_int(*gp) && as_int(*gp) == 42, "get-provenance 42");
    }

    // ── AC5: concurrent FlatAST metadata lock stress (not full eval) ──
    {
        std::println("\n--- AC5: multi-thread FlatAST metadata stress ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "set-code stress");
        auto r = cs.eval("(car (query:find \"a\"))");
        CHECK(r && is_int(*r), "find a");
        const auto id = static_cast<aura::ast::NodeId>(as_int(*r));
        auto* flat = cs.workspace_flat();
        CHECK(flat != nullptr && id < flat->size(), "flat + id");

        std::atomic<int> ok{0};
        std::atomic<int> fail{0};
        constexpr int kThreads = 4;
        constexpr int kIters = 500;
        const auto t0 = clock::now();
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < kIters; ++i) {
                    const auto marker = static_cast<SyntaxMarker>((t + i) % 3);
                    {
                        auto w = flat->begin_metadata_mutation();
                        flat->set_marker(id, marker);
                        flat->set_provenance(id, static_cast<std::uint32_t>(1000 + t * kIters + i));
                    }
                    SyntaxMarker got_m = SyntaxMarker::User;
                    std::uint32_t got_p = 0;
                    {
                        auto rlk = flat->try_acquire_metadata_reader_lock();
                        got_m = flat->marker(id);
                        got_p = flat->provenance(id);
                    }
                    const auto mi = static_cast<int>(got_m);
                    if (mi < 0 || mi > 2 || got_p == 0) {
                        fail.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    ok.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
        CHECK(ms < 5000, std::format("stress finished in {}ms", ms));
        CHECK(fail.load() == 0, std::format("no failures (fail={})", fail.load()));
        CHECK(ok.load() == kThreads * kIters, std::format("all iters ok (got {})", ok.load()));
        std::println("  stress ok={} fail={} in {}ms", ok.load(), fail.load(), ms);
    }

    std::println("\n=== test_marker_metadata_lock_1783: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
