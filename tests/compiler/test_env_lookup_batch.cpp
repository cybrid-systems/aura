// test_env_lookup_batch.cpp — batch driver for Env::lookup family.
// Consolidates 3 issue tests into 1 batch entry (Phase 4+ migration,
// following the test_per_defuse_batch / test_issues_809_817_batch
// precedent in AuraDomainTests.cmake):
//
//   Issue #1858 — Env::lookup depth counter is one-per-frame (not 2× on
//                 parent recursion). Peak depth N, not 2N. (3 ACs)
//   Issue #1860 — Env::lookup_binding / lookup_by_symid depth guard
//                 shared vs cyclic parent_ graph. (3 ACs)
//   Issue #1862 — Env::lookup_by_symid raw walk under single-writer
//                 contract (no env_symid_mtx_ on hot path). (3 ACs)
//
// Pattern: CHECK() macros + RUN_ALL_TESTS() (test_harness.hpp),
// namespace aura_env_lookup_batch, EXCLUDE_FROM_ALL per AuraDomainTests.cmake
// legacy batch convention. Default build skips; granular debug via
// `ninja test_env_lookup_batch` on demand.

#include "test_harness.hpp"

#include <fstream>
#include <initializer_list>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.core;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace aura_env_lookup_batch {

using aura::ast::StringPool;
using aura::compiler::Env;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

// ── Block 1: Issue #1858 (3 ACs) ──
// Original: tests/test_env_lookup_depth_1858.cpp
struct Chain1858 {
    std::vector<std::unique_ptr<Env>> envs; // [0]=leaf, back()=root
    Env* leaf() { return envs.front().get(); }
    Env* root() { return envs.back().get(); }
};

static Chain1858 make_parent_chain_1858(std::size_t n_frames) {
    Chain1858 c;
    c.envs.reserve(n_frames);
    for (std::size_t i = 0; i < n_frames; ++i)
        c.envs.push_back(std::make_unique<Env>());
    for (std::size_t i = 0; i + 1 < n_frames; ++i)
        c.envs[i]->set_parent(c.envs[i + 1].get());
    c.root()->bind("deep-var", make_int(42));
    return c;
}

static void run_1858_depth_one_per_frame() {
    std::println("\n=== Issue #1858: Env::lookup depth one-per-frame ===");

    // AC1: source
    {
        std::println("\n--- AC1: #1858 one-per-frame depth semantics ---");
        std::string src;
        for (const char* p :
             {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read evaluator_env.cpp");
        CHECK(src.find("#1858") != std::string::npos, "cites #1858");
        CHECK(src.find("MAX_ENV_DEPTH") != std::string::npos, "has MAX_ENV_DEPTH");
        CHECK(src.find("1024") != std::string::npos, "limit is 1024 (not blindly doubled)");
        CHECK(src.find("not 2N") != std::string::npos ||
                  src.find("peaks at N") != std::string::npos ||
                  src.find("not halved") != std::string::npos,
              "documents correct N-not-2N semantics");
        CHECK(src.find("g_env_lookup_depth") != std::string::npos, "depth counter present");
        CHECK(src.find("parent_->lookup") != std::string::npos, "recursive parent walk");
    }

    // AC2: chain of 600 > 512 (under correct semantics peak=600; would fail at 1200)
    {
        std::println("\n--- AC2: 600-frame parent chain still finds binding ---");
        constexpr std::size_t kChain = 600;
        auto chain = make_parent_chain_1858(kChain);
        auto v = chain.leaf()->lookup("deep-var");
        CHECK(v.has_value(), "lookup found deep-var through 600 parents");
        if (v)
            CHECK(is_int(*v) && as_int(*v) == 42, "value is 42");
        auto miss = chain.leaf()->lookup("no-such-binding-xyz");
        CHECK(!miss.has_value(), "missing name → nullopt after full walk");
    }

    // AC3: depth balanced after deep walk (RAII guard)
    {
        std::println("\n--- AC3: second deep lookup still works (depth reset) ---");
        auto chain = make_parent_chain_1858(400);
        auto a = chain.leaf()->lookup("deep-var");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "first walk ok");
        auto b = chain.leaf()->lookup("deep-var");
        CHECK(b && is_int(*b) && as_int(*b) == 42, "second walk ok (RAII restored depth)");
        auto short_c = make_parent_chain_1858(3);
        auto c = short_c.leaf()->lookup("deep-var");
        CHECK(c && is_int(*c) && as_int(*c) == 42, "short chain ok");
    }
}

// ── Block 2: Issue #1860 (3 ACs) ──
// Original: tests/test_env_lookup_binding_depth_1860.cpp
static void run_1860_binding_depth_cycle() {
    std::println("\n=== Issue #1860: lookup_binding / lookup_by_symid depth guard ===");

    // AC1: source
    {
        std::println("\n--- AC1: lookup_binding depth guard ---");
        std::string src;
        for (const char* p :
             {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read evaluator_env.cpp");
        CHECK(src.find("#1860") != std::string::npos, "cites #1860");
        auto pos = src.find("Env::lookup_binding");
        CHECK(pos != std::string::npos, "lookup_binding present");
        auto win = src.substr(pos, 900);
        CHECK(win.find("env_lookup_enter") != std::string::npos ||
                  win.find("g_env_lookup_depth") != std::string::npos ||
                  win.find("EnvLookupDepthGuard") != std::string::npos,
              "uses shared depth guard");
        CHECK(win.find("parent_->lookup_binding") != std::string::npos, "still walks parent_");
        auto spos = src.find("Env::lookup_by_symid");
        CHECK(spos != std::string::npos, "lookup_by_symid present");
        auto swin = src.substr(spos, 900);
        CHECK(swin.find("env_lookup_enter") != std::string::npos ||
                  swin.find("EnvLookupDepthGuard") != std::string::npos,
              "lookup_by_symid depth guarded");
    }

    // AC2: normal chain
    {
        std::println("\n--- AC2: lookup_binding parent chain ---");
        Env root;
        root.bind("x", make_int(7));
        Env mid;
        mid.set_parent(&root);
        Env leaf;
        leaf.set_parent(&mid);
        auto v = leaf.lookup_binding("x");
        CHECK(v.has_value(), "lookup_binding found x");
        if (v)
            CHECK(is_int(*v) && as_int(*v) == 7, "value is 7");
        auto v2 = leaf.lookup("x");
        CHECK(v2 && is_int(*v2) && as_int(*v2) == 7, "lookup after binding still ok");
    }

    // AC3: cyclic parent_
    {
        std::println("\n--- AC3: cyclic parent_ does not overflow ---");
        Env a;
        Env b;
        a.set_parent(&b);
        b.set_parent(&a); // cycle
        auto r = a.lookup_binding("cycle-miss");
        CHECK(!r.has_value(), "cycle → nullopt (depth guard)");
        auto r2 = a.lookup("cycle-miss");
        CHECK(!r2.has_value(), "lookup also bails on cycle");
        Env root;
        root.bind("y", make_int(1));
        Env leaf;
        leaf.set_parent(&root);
        auto y = leaf.lookup_binding("y");
        CHECK(y && is_int(*y) && as_int(*y) == 1, "post-cycle acyclic lookup_binding ok");
    }
}

// ── Block 3: Issue #1862 (3 ACs) ──
// Original: tests/test_env_lookup_by_symid_race_1862.cpp
static void run_1862_lookup_by_symid_single_writer() {
    std::println("\n=== Issue #1862: lookup_by_symid single-writer contract ===");

    // AC1: source contract
    {
        std::println("\n--- AC1: #1862 lookup_by_symid single-writer docs ---");
        auto env_cpp =
            read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
        auto env_ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
        CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
        CHECK(!env_ixx.empty(), "read evaluator.ixx");
        CHECK(env_cpp.find("#1862") != std::string::npos, "cites #1862");
        auto pos = env_cpp.find("Env::lookup_by_symid");
        CHECK(pos != std::string::npos, "lookup_by_symid present");
        auto win = env_cpp.substr(pos > 500 ? pos - 500 : 0, 1100);
        CHECK(win.find("#1862") != std::string::npos, "lookup_by_symid window cites #1862");
        CHECK(win.find("single-writer") != std::string::npos ||
                  win.find("#1861") != std::string::npos,
              "ties to single-writer / #1861");
        CHECK(win.find("env_symid_mtx_") != std::string::npos ||
                  win.find("no shared_lock") != std::string::npos ||
                  win.find("no env_symid") != std::string::npos,
              "documents no env_symid_mtx_ / no shared_lock");
        CHECK(env_ixx.find("env_symid_mtx_{") == std::string::npos &&
                  env_ixx.find("env_symid_mtx_;") == std::string::npos &&
                  env_ixx.find("mutex env_symid") == std::string::npos,
              "no env_symid_mtx_ member declaration");
        CHECK(env_ixx.find("#1862") != std::string::npos, "Env class cites #1862 sibling");
        auto bpos = env_cpp.find("Env::bind_symid_with_linear_state");
        CHECK(bpos != std::string::npos, "bind_symid_with_linear_state present");
        auto bwin = env_cpp.substr(bpos > 350 ? bpos - 350 : 0, 700);
        CHECK(bwin.find("#1862") != std::string::npos || bwin.find("#1861") != std::string::npos,
              "bind_symid path cites race sibling");
    }

    // AC2: sequential bind_symid + lookup_by_symid
    {
        std::println("\n--- AC2: sequential bind_symid then lookup_by_symid ---");
        StringPool pool;
        auto sx = pool.intern("x");
        auto sy = pool.intern("y");
        Env root;
        root.set_pool(&pool);
        root.bind_symid(sx, make_int(10));
        root.bind_symid(sy, make_int(20));

        auto vx = root.lookup_by_symid(sx);
        auto vy = root.lookup_by_symid(sy);
        CHECK(vx && is_int(*vx) && as_int(*vx) == 10, "local lookup_by_symid x");
        CHECK(vy && is_int(*vy) && as_int(*vy) == 20, "local lookup_by_symid y");

        auto vsx = root.lookup("x");
        CHECK(vsx && is_int(*vsx) && as_int(*vsx) == 10, "string mirror x after bind_symid");

        Env leaf;
        leaf.set_parent(&root);
        auto vxp = leaf.lookup_by_symid(sx);
        CHECK(vxp && is_int(*vxp) && as_int(*vxp) == 10, "parent chain lookup_by_symid x");
    }

    // AC3: rebind + missing
    {
        std::println("\n--- AC3: rebind most-recent-wins; missing nullopt ---");
        StringPool pool;
        auto s = pool.intern("shadow");
        Env env;
        env.set_pool(&pool);
        env.bind_symid(s, make_int(1));
        env.bind_symid(s, make_int(2));
        auto v = env.lookup_by_symid(s);
        CHECK(v && is_int(*v) && as_int(*v) == 2, "most-recent-wins after re-bind_symid");
        auto miss = env.lookup_by_symid(pool.intern("nope"));
        CHECK(!miss.has_value(), "missing SymId → nullopt");
        for (int i = 0; i < 64; ++i) {
            auto name = std::string("n") + std::to_string(i);
            env.bind_symid(pool.intern(name), make_int(i));
        }
        auto last = env.lookup_by_symid(pool.intern("n63"));
        CHECK(last && is_int(*last) && as_int(*last) == 63, "post-growth lookup still correct");
    }
}

} // namespace aura_env_lookup_batch

int main() {
    aura_env_lookup_batch::run_1858_depth_one_per_frame();
    aura_env_lookup_batch::run_1860_binding_depth_cycle();
    aura_env_lookup_batch::run_1862_lookup_by_symid_single_writer();
    return RUN_ALL_TESTS();
}