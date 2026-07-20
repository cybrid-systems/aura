// test_env_batch.cpp
// B pilot #18 (after arena in e1d6ec65): consolidated env family
// — Issues #1861 + #1863 + #1754 + #1859 + #1482 + #1868 + #1869
// (Env::bind_with_linear_state single-writer + Env::bindings_with_names
// single-writer + is_env_frame_stale vs invalid_id split + SoA iterative
// parent_id walk + SymId PRIMARY storage + EnvView zero-copy lifetime +
// EnvView depth guard) into one batch driver.
//
// NOTE: test_env_lookup_batch.cpp is intentionally NOT included — already
// a batch entry in cmake/AuraDomainTests.cmake:32-34 (Issue #1858/#1860/#1862,
// EXCLUDE_FROM_ALL, created by earlier consolidation waves).
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention.
// EXCLUDE_FROM_ALL — default build skips; on-demand 'ninja test_env_batch'.

#include "test_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.core;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_env_batch {

using aura::ast::StringPool;
using aura::ast::SymId;
using aura::compiler::CompilerService;
using aura::compiler::Env;
using aura::compiler::EnvId;
using aura::compiler::Evaluator;
using aura::compiler::make_env_view;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

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

// ── Issue #1861 — Env::bind_with_linear_state single-writer ──
static void run_1861_source_contract() {
    std::println("\n--- AC1 (#1861): #1861 single-writer / intern docs ---");
    auto env_ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    auto env_cpp =
        read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
    auto ast_ixx = read_first({"src/core/ast.ixx", "../src/core/ast.ixx"});
    CHECK(!env_ixx.empty(), "read evaluator.ixx");
    CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
    CHECK(!ast_ixx.empty(), "read ast.ixx");
    CHECK(env_ixx.find("#1861") != std::string::npos, "Env cites #1861");
    CHECK(env_ixx.find("single-writer") != std::string::npos, "documents single-writer");
    CHECK(env_cpp.find("#1861") != std::string::npos, "bind_with_linear_state cites #1861");
    auto bpos = env_cpp.find("Env::bind_with_linear_state");
    CHECK(bpos != std::string::npos, "bind_with_linear_state present");
    auto win = env_cpp.substr(bpos > 400 ? bpos - 400 : 0, 900);
    CHECK(win.find("single-writer") != std::string::npos ||
              win.find("Not atomic") != std::string::npos ||
              win.find("not locked") != std::string::npos,
          "bind path documents no lock / single-writer");
    CHECK(win.find("not thread-safe") != std::string::npos ||
              win.find("Not thread-safe") != std::string::npos ||
              env_cpp.find("StringPool::intern") != std::string::npos,
          "documents intern not thread-safe");
    CHECK(env_cpp.find("safe on shared pool") == std::string::npos,
          "removed misleading 'safe on shared pool' claim");
    CHECK(ast_ixx.find("#1861") != std::string::npos, "StringPool cites #1861");
    CHECK(ast_ixx.find("Not thread-safe") != std::string::npos ||
              ast_ixx.find("not thread-safe") != std::string::npos,
          "StringPool documents intern not TS");
}

static void run_1861_sequential_bind() {
    std::println("\n--- AC2 (#1861): sequential bind_with_linear_state dual path ---");
    StringPool pool;
    Env env;
    env.set_pool(&pool);
    env.bind_with_linear_state("x", make_int(42), /*state=*/0);
    env.bind_with_linear_state("y", make_int(7), /*state=*/0);

    auto vx = env.lookup("x");
    CHECK(vx && is_int(*vx) && as_int(*vx) == 42, "lookup x via string");
    auto vy = env.lookup("y");
    CHECK(vy && is_int(*vy) && as_int(*vy) == 7, "lookup y via string");

    auto sx = pool.find_by_name("x");
    auto sy = pool.find_by_name("y");
    CHECK(sx.has_value() && sy.has_value(), "pool has x and y");
    if (sx && sy) {
        auto vsx = env.lookup_by_symid(*sx);
        auto vsy = env.lookup_by_symid(*sy);
        CHECK(vsx && is_int(*vsx) && as_int(*vsx) == 42, "lookup_by_symid x");
        CHECK(vsy && is_int(*vsy) && as_int(*vsy) == 7, "lookup_by_symid y");
    }
    env.bind_with_linear_state("x", make_int(99), /*state=*/0);
    auto vx2 = env.lookup("x");
    CHECK(vx2 && is_int(*vx2) && as_int(*vx2) == 99, "rebinding x updates string lookup");
    if (sx) {
        auto vsx2 = env.lookup_by_symid(*sx);
        CHECK(vsx2 && is_int(*vsx2) && as_int(*vsx2) == 99, "rebinding x updates symid lookup");
    }
}

static void run_1861_multi_name_binds() {
    std::println("\n--- AC3 (#1861): multi-name binds remain look-uppable ---");
    Env env;
    for (int i = 0; i < 32; ++i) {
        auto name = std::string("v") + std::to_string(i);
        env.bind(name, make_int(i));
    }
    for (int i = 0; i < 32; ++i) {
        auto name = std::string("v") + std::to_string(i);
        auto v = env.lookup(name);
        CHECK(v && is_int(*v) && as_int(*v) == i,
              (std::string("lookup ") + name + " == " + std::to_string(i)).c_str());
    }
    CHECK(!env.lookup("nope").has_value(), "missing → nullopt");
}

// ── Issue #1863 — Env::bindings_with_names single-writer ──
static void run_1863_source_contract() {
    std::println("\n--- AC1 (#1863): #1863 bindings_with_names single-writer docs ---");
    auto env_cpp =
        read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
    auto env_ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
    CHECK(!env_ixx.empty(), "read evaluator.ixx");
    CHECK(env_cpp.find("#1863") != std::string::npos, "cites #1863");
    auto pos = env_cpp.find("Env::bindings_with_names");
    CHECK(pos != std::string::npos, "bindings_with_names present");
    auto win = env_cpp.substr(pos > 600 ? pos - 600 : 0, 1400);
    CHECK(win.find("#1863") != std::string::npos, "window cites #1863");
    CHECK(win.find("single-writer") != std::string::npos || win.find("#1861") != std::string::npos,
          "ties to single-writer / #1861");
    CHECK(win.find("env_symid_mtx_") != std::string::npos ||
              win.find("no shared_lock") != std::string::npos ||
              win.find("unlocked") != std::string::npos,
          "documents no lock");
    CHECK(win.find("resolve") != std::string::npos, "mentions pool resolve");
    CHECK(env_ixx.find("env_symid_mtx_{") == std::string::npos &&
              env_ixx.find("env_symid_mtx_;") == std::string::npos &&
              env_ixx.find("mutex env_symid") == std::string::npos,
          "no env_symid_mtx_ member declaration");
    CHECK(env_ixx.find("#1863") != std::string::npos, "Env class cites #1863 sibling");
}

static void run_1863_sequential_bind() {
    std::println("\n--- AC2 (#1863): sequential bind_symid → bindings_with_names ---");
    StringPool pool;
    auto sx = pool.intern("alpha");
    auto sy = pool.intern("beta");
    Env env;
    env.set_pool(&pool);
    env.bind_symid(sx, make_int(1));
    env.bind_symid(sy, make_int(2));

    auto named = env.bindings_with_names();
    CHECK(named.size() == 2, "two named bindings");
    CHECK(named[0].first == "alpha", "first name alpha");
    CHECK(named[1].first == "beta", "second name beta");
    CHECK(is_int(named[0].second) && as_int(named[0].second) == 1, "alpha value 1");
    CHECK(is_int(named[1].second) && as_int(named[1].second) == 2, "beta value 2");

    for (int i = 0; i < 40; ++i) {
        auto n = std::string("v") + std::to_string(i);
        env.bind_symid(pool.intern(n), make_int(100 + i));
    }
    named = env.bindings_with_names();
    CHECK(named.size() == 42, "42 after growth");
    auto it =
        std::find_if(named.begin(), named.end(), [](const auto& p) { return p.first == "v39"; });
    CHECK(it != named.end(), "finds v39");
    if (it != named.end())
        CHECK(is_int(it->second) && as_int(it->second) == 139, "v39 value 139");
}

static void run_1863_no_pool_fallback() {
    std::println("\n--- AC3 (#1863): no-pool @symid fallback; empty env ---");
    Env empty;
    CHECK(empty.bindings_with_names().empty(), "empty env → empty vector");

    Env env;
    env.bind_symid(/*s=*/7, make_int(70));
    env.bind_symid(/*s=*/9, make_int(90));
    auto named = env.bindings_with_names();
    CHECK(named.size() == 2, "two fallback names");
    CHECK(named[0].first == "@symid:7", "fallback @symid:7");
    CHECK(named[1].first == "@symid:9", "fallback @symid:9");
    CHECK(is_int(named[0].second) && as_int(named[0].second) == 70, "value 70");
    CHECK(is_int(named[1].second) && as_int(named[1].second) == 90, "value 90");
}

// ── Issue #1754 — is_env_frame_stale vs invalid_id split ──
static void run_1754_source() {
    std::println("\n--- AC1 (#1754): source cites #1754 + invalid_id ---");
    std::string env;
    for (const char* p : {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
        env = read_file(p);
        if (!env.empty())
            break;
    }
    CHECK(!env.empty(), "read evaluator_env.cpp");
    CHECK(env.find("#1754") != std::string::npos, "cites #1754");
    CHECK(env.find("is_env_frame_invalid_id") != std::string::npos, "uses is_env_frame_invalid_id");

    std::string ixx;
    for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
        ixx = read_file(p);
        if (!ixx.empty())
            break;
    }
    CHECK(!ixx.empty() && ixx.find("is_env_frame_invalid_id") != std::string::npos,
          "invalid_id declared in evaluator.ixx");
}

static void run_1754_null_oob() {
    std::println("\n--- AC2 (#1754): NULL/OOB are invalid_id, not stale ---");
    Evaluator ev;
    CHECK(ev.is_env_frame_invalid_id(NULL_ENV_ID), "NULL invalid_id");
    CHECK(!ev.is_env_frame_stale(NULL_ENV_ID), "NULL not stale");
    CHECK(ev.is_env_frame_invalid_id(999999), "OOB invalid_id");
    CHECK(!ev.is_env_frame_stale(999999), "OOB not stale");
    CHECK(!ev.is_valid_env_id(NULL_ENV_ID), "NULL not valid_env_id");
    CHECK(!ev.is_valid_env_id(999999), "OOB not valid_env_id");
}

static void run_1754_fresh_live() {
    std::println("\n--- AC3 (#1754): fresh frame neither invalid_id nor stale ---");
    Evaluator ev;
    EnvId id = ev.alloc_env_frame();
    CHECK(id != NULL_ENV_ID, "alloc");
    CHECK(!ev.is_env_frame_invalid_id(id), "live not invalid_id");
    CHECK(!ev.is_env_frame_stale(id), "fresh not stale");
    CHECK(ev.is_valid_env_id(id), "live is valid_env_id");
}

static void run_1754_version_stale() {
    std::println("\n--- AC4 (#1754): defuse bump → stale, still valid id ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    EnvId id = ev.alloc_env_frame();
    CHECK(!ev.is_env_frame_stale(id), "fresh");
    for (int i = 0; i < 3; ++i)
        ev.bump_defuse_version_for_test();
    CHECK(!ev.is_env_frame_invalid_id(id), "still a valid slot");
    CHECK(ev.is_env_frame_stale(id), "version-stale after defuse bump");
}

// ── Issue #1859 — Env::lookup SoA iterative walk ──
static EnvId make_soa_chain_1859(Evaluator& ev, std::size_t n_frames) {
    EnvId root = ev.alloc_env_frame(NULL_ENV_ID);
    ev.env_frame_mut(root).bind("soa-var", make_int(99));
    EnvId cur = root;
    for (std::size_t i = 1; i < n_frames; ++i)
        cur = ev.alloc_env_frame(cur);
    return cur;
}

static void run_1859_source() {
    std::println("\n--- AC1 (#1859): iterative SoA walk ---");
    std::string src;
    for (const char* p : {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
        src = read_file(p);
        if (!src.empty())
            break;
    }
    CHECK(!src.empty(), "read evaluator_env.cpp");
    CHECK(src.find("#1859") != std::string::npos, "cites #1859");
    auto pos = src.find("parent_id_ != NULL_ENV_ID");
    CHECK(pos != std::string::npos, "SoA parent_id_ branch present");
    auto win = src.substr(pos > 800 ? pos - 800 : 0, 3200);
    CHECK(win.find("while (cur != NULL_ENV_ID") != std::string::npos ||
              win.find("while (cur != NULL_ENV_ID &&") != std::string::npos,
          "iterative while over parent_id chain");
    CHECK(win.find("hops") != std::string::npos, "hop counter");
    CHECK(win.find("MAX_ENV_DEPTH") != std::string::npos, "bounded by MAX_ENV_DEPTH");
    auto wpos = win.find("while (cur != NULL_ENV_ID");
    CHECK(wpos != std::string::npos, "while present for code-path scan");
    if (wpos != std::string::npos) {
        auto body = win.substr(wpos);
        std::string code;
        for (std::size_t i = 0; i < body.size();) {
            auto nl = body.find('\n', i);
            auto line = body.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
            auto cmt = line.find("//");
            if (cmt != std::string::npos)
                line = line.substr(0, cmt);
            code += line;
            code += '\n';
            if (nl == std::string::npos)
                break;
            i = nl + 1;
        }
        CHECK(code.find("tmp.lookup") == std::string::npos, "no tmp.lookup in code");
        CHECK(code.find("Env tmp") == std::string::npos, "no Env tmp in code");
    }
    CHECK(win.find("shared_lock") != std::string::npos, "holds shared_lock once");
    CHECK(win.find("iteratively") != std::string::npos ||
              win.find("not recursive") != std::string::npos,
          "documents iterative fix");
}

static void run_1859_short_chain() {
    std::println("\n--- AC2 (#1859): short SoA parent_id chain ---");
    Evaluator ev;
    EnvId leaf_id = make_soa_chain_1859(ev, 4);
    Env leaf;
    leaf.set_owner(&ev);
    leaf.set_parent_id(leaf_id);
    auto v = leaf.lookup("soa-var");
    CHECK(v.has_value(), "found soa-var via short SoA chain");
    if (v)
        CHECK(is_int(*v) && as_int(*v) == 99, "value is 99");
    auto miss = leaf.lookup("no-such-soa-name");
    CHECK(!miss.has_value(), "missing name → nullopt");
}

static void run_1859_deep_chain() {
    std::println("\n--- AC3 (#1859): 600-hop SoA chain ---");
    Evaluator ev;
    EnvId leaf_id = make_soa_chain_1859(ev, 600);
    Env leaf;
    leaf.set_owner(&ev);
    leaf.set_parent_id(leaf_id);
    auto v = leaf.lookup("soa-var");
    CHECK(v.has_value(), "found soa-var through 600 SoA hops");
    if (v)
        CHECK(is_int(*v) && as_int(*v) == 99, "deep value is 99");
    auto v2 = leaf.lookup("soa-var");
    CHECK(v2 && is_int(*v2) && as_int(*v2) == 99, "second deep SoA walk ok");
}

// ── Issue #1482 — Env::bind_symid PRIMARY storage ──
static void run_1482_bindings_empty() {
    std::println("\n--- AC1 (#1482): bind_symid leaves legacy bindings_ empty ---");
    Env env;
    StringPool pool;
    env.set_pool(&pool);

    const auto s_foo = pool.intern("foo");
    const auto s_bar = pool.intern("bar");
    env.bind_symid(s_foo, make_int(1));
    env.bind_symid(s_bar, make_int(2));

    CHECK(env.bindings().empty(),
          "AC1: Env::bind_symid leaves legacy bindings_ empty (Commit 1 eager-mirror drop)");

    const auto symid_view = env.bindings_symid_iter();
    CHECK(symid_view.size() == 2, "AC1: bindings_symid_iter() returns 2 entries");
    CHECK(symid_view[0].first == s_foo && as_int(symid_view[0].second) == 1,
          "AC1: symid_view[0] == (foo, 1)");
    CHECK(symid_view[1].first == s_bar && as_int(symid_view[1].second) == 2,
          "AC1: symid_view[1] == (bar, 2)");
}

static void run_1482_binding_index_untouched() {
    std::println("\n--- AC2 (#1482): bind_symid leaves binding_index_ untouched ---");
    Env env;
    StringPool pool;
    env.set_pool(&pool);

    env.bind_symid(pool.intern("alpha"), make_int(10));
    env.bind_symid(pool.intern("beta"), make_int(20));

    const auto legacy_lookup = env.lookup("alpha");
    CHECK(!legacy_lookup.has_value(),
          "AC2: legacy string-keyed lookup(\"alpha\") misses after bind_symid-only");

    const auto symid_lookup = env.lookup_by_symid(pool.intern("alpha"));
    CHECK(symid_lookup.has_value() && as_int(*symid_lookup) == 10,
          "AC2: lookup_by_symid(alpha) returns 10");
}

static void run_1482_symid_iter_canonical() {
    std::println("\n--- AC3 (#1482): bindings_symid_iter() == bindings_symid() ---");
    Env env;
    StringPool pool;
    env.set_pool(&pool);

    env.bind_symid(pool.intern("k1"), make_int(100));
    env.bind_symid(pool.intern("k2"), make_int(200));

    const auto iter_view = env.bindings_symid_iter();
    const auto fn_view = env.bindings_symid();
    CHECK(iter_view.size() == fn_view.size(),
          "AC3: bindings_symid_iter().size() == bindings_symid().size()");
    CHECK(iter_view.data() == fn_view.data(),
          "AC3: bindings_symid_iter() and bindings_symid() share backing storage");
}

static void run_1482_pool_resolves_names() {
    std::println("\n--- AC4 (#1482): bindings_with_names() resolves names via pool_ ---");
    Env env;
    StringPool pool;
    env.set_pool(&pool);

    const auto s_x = pool.intern("x");
    const auto s_y = pool.intern("y");
    env.bind_symid(s_x, make_int(7));
    env.bind_symid(s_y, make_int(8));

    const auto with_names = env.bindings_with_names();
    CHECK(with_names.size() == 2, "AC4: bindings_with_names() returns 2 entries");
    if (with_names.size() == 2) {
        CHECK(with_names[0].first == "x" && as_int(with_names[0].second) == 7,
              "AC4: with_names[0] == (\"x\", 7)");
        CHECK(with_names[1].first == "y" && as_int(with_names[1].second) == 8,
              "AC4: with_names[1] == (\"y\", 8)");
    }
}

static void run_1482_no_pool_fallback() {
    std::println("\n--- AC5 (#1482): bindings_with_names() falls back to @<symid:N> ---");
    Env env;
    // No set_pool — pool_ stays nullptr.

    const auto s_sym1 = aura::ast::SymId{42};
    const auto s_sym2 = aura::ast::SymId{43};
    env.bind_symid(s_sym1, make_int(11));
    env.bind_symid(s_sym2, make_int(22));

    const auto with_names = env.bindings_with_names();
    CHECK(with_names.size() == 2, "AC5: bindings_with_names() with pool_=null returns 2 entries");
    if (with_names.size() == 2) {
        const auto expected_0 = std::format("@<symid:{}>", static_cast<std::uint32_t>(s_sym1));
        const auto expected_1 = std::format("@<symid:{}>", static_cast<std::uint32_t>(s_sym2));
        CHECK(with_names[0].first == expected_0, "AC5: no-pool fallback uses @<symid:N> format");
        CHECK(with_names[1].first == expected_1, "AC5: no-pool fallback uses @<symid:N> format");
    }
}

static void run_1482_alloc_env_frame_from_env() {
    std::println("\n--- AC6 (#1482): alloc_env_frame_from_env frame's bindings_ stays empty ---");
    Evaluator ev;
    StringPool pool;
    ev.set_workspace_pool(&pool);

    Env src;
    src.set_pool(&pool);
    const auto s_p = pool.intern("p");
    const auto s_q = pool.intern("q");
    src.bind_symid(s_p, make_int(100));
    src.bind_symid(s_q, make_int(200));

    const EnvId fid = ev.alloc_env_frame_from_env(src);
    CHECK(fid != NULL_ENV_ID, "AC6: alloc_env_frame_from_env returns valid EnvId");

    const auto& fr = ev.env_frame(fid);
    CHECK(fr.bindings_.empty(),
          "AC6: post-Commit-2 frame.bindings_ stays empty after alloc_env_frame_from_env");
    CHECK(fr.bindings_symid_.size() == 2, "AC6: frame.bindings_symid_ has 2 entries");
}

static void run_1482_desync_counter() {
    std::println("\n--- AC7 (#1482): bump_envframe_desync_detected fires for post-Commit 1/2 ---");
    Evaluator ev;
    StringPool pool;
    ev.set_workspace_pool(&pool);

    const auto before = ev.get_envframe_desync_detected();

    Env src;
    src.set_pool(&pool);
    src.bind_symid(pool.intern("m"), make_int(1));
    src.bind_symid(pool.intern("n"), make_int(2));
    src.bind_symid(pool.intern("o"), make_int(3));

    for (int i = 0; i < 3; ++i) {
        (void)ev.alloc_env_frame_from_env(src);
    }

    const auto after = ev.get_envframe_desync_detected();
    CHECK(after >= before + 3, "AC7: 3 alloc_env_frame_from_env bump envframe_desync by >= 3");
}

static void run_1482_perf_microbench() {
    std::println("\n--- AC8 (#1482): perf microbench bindings_symid_iter vs with_names ---");
    Env env;
    StringPool pool;
    env.set_pool(&pool);

    std::vector<aura::ast::SymId> syms;
    syms.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        const auto name = std::format("var_{}", i);
        const auto s = pool.intern(name);
        env.bind_symid(s, make_int(i));
        syms.push_back(s);
    }

    for (int w = 0; w < 100; ++w) {
        (void)env.bindings_symid_iter();
        (void)env.bindings_with_names();
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t iter_sum = 0;
    for (int rep = 0; rep < 10000; ++rep) {
        const auto view = env.bindings_symid_iter();
        for (const auto& [s, v] : view) {
            iter_sum += static_cast<std::uint64_t>(s);
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto iter_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    const auto t2 = std::chrono::steady_clock::now();
    std::uint64_t names_sum = 0;
    for (int rep = 0; rep < 10000; ++rep) {
        const auto with_names = env.bindings_with_names();
        for (const auto& [name, v] : with_names) {
            names_sum += static_cast<std::uint64_t>(name.size());
        }
    }
    const auto t3 = std::chrono::steady_clock::now();
    const auto names_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();

    CHECK(iter_sum != 0, "AC8: iter_sum is non-zero");
    CHECK(names_sum != 0, "AC8: names_sum is non-zero");
    CHECK(names_ns > iter_ns,
          "AC8: bindings_with_names() slower than bindings_symid_iter() (canonical hot-path)");

    const auto ratio =
        (iter_ns > 0) ? (static_cast<double>(names_ns) / static_cast<double>(iter_ns)) : 0.0;
    std::println("  PERF: iter={} ns, names={} ns, ratio={:.2f}x", iter_ns, names_ns, ratio);
}

// ── Issue #1868 — EnvView zero-copy lifetime ──
static void run_1868_source_contract() {
    std::println("\n--- AC1 (#1868): #1868 EnvView lifetime docs ---");
    auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    auto env_cpp =
        read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
    CHECK(!ixx.empty(), "read evaluator.ixx");
    CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
    CHECK(ixx.find("#1868") != std::string::npos, "EnvView cites #1868");
    auto epos = ixx.find("export struct EnvView");
    CHECK(epos != std::string::npos, "EnvView present");
    auto ewin = ixx.substr(epos > 800 ? epos - 800 : 0, 1600);
    CHECK(ewin.find("zero-copy") != std::string::npos || ewin.find("span") != std::string::npos,
          "documents spans");
    CHECK(ewin.find("invalid") != std::string::npos || ewin.find("UAF") != std::string::npos ||
              ewin.find("realloc") != std::string::npos,
          "documents invalidation / realloc");
    CHECK(ewin.find("single-writer") != std::string::npos ||
              ewin.find("#1861") != std::string::npos,
          "ties to single-writer / #1861");
    CHECK(env_cpp.find("#1868") != std::string::npos, "make_env_view cites #1868");
    auto mpos = env_cpp.find("make_env_view");
    CHECK(mpos != std::string::npos, "make_env_view present");
    auto mwin = env_cpp.substr(mpos > 400 ? mpos - 400 : 0, 900);
    CHECK(mwin.find("zero-copy") != std::string::npos || mwin.find("dangle") != std::string::npos ||
              mwin.find("bind") != std::string::npos,
          "make_env_view documents lifetime");
    CHECK(ixx.find("string_bindings_owned") == std::string::npos, "no defensive owned copy field");
}

static void run_1868_sequential_view() {
    std::println("\n--- AC2 (#1868): sequential make_env_view + lookup ---");
    Env env;
    env.bind("foo", make_int(42));
    env.bind("bar", make_int(7));
    auto view = make_env_view(env);
    CHECK(view.size() == 2, "two string bindings");
    auto foo = view.lookup("foo");
    CHECK(foo && is_int(*foo) && as_int(*foo) == 42, "lookup foo");
    auto bar = view.lookup("bar");
    CHECK(bar && is_int(*bar) && as_int(*bar) == 7, "lookup bar");
    auto miss = view.lookup("nope");
    CHECK(!miss.has_value(), "missing → nullopt");
    Env child;
    child.set_parent(&env);
    auto cv = make_env_view(child);
    auto via = cv.lookup("foo");
    CHECK(via && is_int(*via) && as_int(*via) == 42, "parent walk via view");
}

static void run_1868_remake() {
    std::println("\n--- AC3 (#1868): remake view after bind ---");
    Env env;
    env.bind("a", make_int(1));
    auto v1 = make_env_view(env);
    CHECK(v1.lookup("a") && as_int(*v1.lookup("a")) == 1, "v1 sees a");
    for (int i = 0; i < 64; ++i)
        env.bind(std::string("k") + std::to_string(i), make_int(i));
    auto v2 = make_env_view(env);
    CHECK(v2.size() >= 65, "v2 sees grown bindings");
    auto k63 = v2.lookup("k63");
    CHECK(k63 && is_int(*k63) && as_int(*k63) == 63, "v2 lookup k63");
    CHECK(v2.lookup("a") && as_int(*v2.lookup("a")) == 1, "v2 still finds a");
}

// ── Issue #1869 — EnvView lookup depth guard ──
static void run_1869_source() {
    std::println("\n--- AC1 (#1869): EnvView lookup depth guard ---");
    auto src = read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
    CHECK(!src.empty(), "read evaluator_env.cpp");
    CHECK(src.find("#1869") != std::string::npos, "cites #1869");
    auto pos = src.find("EnvView::lookup(const std::string");
    if (pos == std::string::npos)
        pos = src.find("std::optional<EvalValue> EnvView::lookup");
    CHECK(pos != std::string::npos, "EnvView::lookup present");
    auto win = src.substr(pos, 2800);
    CHECK(win.find("env_lookup_enter") != std::string::npos, "uses env_lookup_enter");
    CHECK(win.find("EnvLookupDepthGuard") != std::string::npos, "uses EnvLookupDepthGuard");
    CHECK(win.find("EnvView::lookup_by_intern") != std::string::npos, "lookup_by_intern present");
    CHECK(win.find("EnvView::lookup_by_symid") != std::string::npos, "lookup_by_symid present");
    std::size_t count = 0;
    for (std::size_t i = 0; (i = win.find("env_lookup_enter", i)) != std::string::npos; i += 1)
        ++count;
    CHECK(count >= 3, "all three EnvView lookups enter depth");
}

static void run_1869_normal_chain() {
    std::println("\n--- AC2 (#1869): EnvView parent chain ---");
    Env root;
    root.bind("x", make_int(11));
    Env mid;
    mid.set_parent(&root);
    Env leaf;
    leaf.set_parent(&mid);
    auto view = make_env_view(leaf);
    auto v = view.lookup("x");
    CHECK(v && is_int(*v) && as_int(*v) == 11, "view.lookup through parents");
    leaf.bind("x", make_int(99));
    auto view2 = make_env_view(leaf);
    auto v2 = view2.lookup("x");
    CHECK(v2 && is_int(*v2) && as_int(*v2) == 99, "local shadow wins");
    StringPool pool;
    auto sx = pool.intern("y");
    root.set_pool(&pool);
    root.bind_symid(sx, make_int(5));
    auto vs = make_env_view(leaf).lookup_by_symid(sx);
    CHECK(vs && is_int(*vs) && as_int(*vs) == 5, "lookup_by_symid via parents");
    auto vi = make_env_view(leaf).lookup_by_intern("y", &pool);
    CHECK(vi && is_int(*vi) && as_int(*vi) == 5, "lookup_by_intern via parents");
}

static void run_1869_cyclic_parent() {
    std::println("\n--- AC3 (#1869): cyclic parent_ does not overflow ---");
    Env a;
    Env b;
    a.set_parent(&b);
    b.set_parent(&a);
    auto va = make_env_view(a);
    auto r = va.lookup("cycle-miss");
    CHECK(!r.has_value(), "cycle → nullopt via view.lookup");
    auto r2 = va.lookup_by_symid(1);
    CHECK(!r2.has_value(), "cycle → nullopt via lookup_by_symid");
    StringPool pool;
    auto r3 = va.lookup_by_intern("cycle-miss", &pool);
    CHECK(!r3.has_value(), "cycle → nullopt via lookup_by_intern");
    Env root;
    root.bind("z", make_int(3));
    Env leaf;
    leaf.set_parent(&root);
    auto z = make_env_view(leaf).lookup("z");
    CHECK(z && is_int(*z) && as_int(*z) == 3, "post-cycle acyclic view ok");
}

} // namespace aura_env_batch

int main() {
    using namespace aura_env_batch;
    std::println(
        "=== Env batch: #1861 + #1863 + #1754 + #1859 + #1482 + #1868 + #1869 (27 ACs total) ===");
    std::println("(test_env_lookup_batch.cpp NOT included — already a batch entry in");
    std::println(" cmake/AuraDomainTests.cmake:32-34 from earlier consolidation waves)");
    run_1861_source_contract();
    run_1861_sequential_bind();
    run_1861_multi_name_binds();
    run_1863_source_contract();
    run_1863_sequential_bind();
    run_1863_no_pool_fallback();
    run_1754_source();
    run_1754_null_oob();
    run_1754_fresh_live();
    run_1754_version_stale();
    run_1859_source();
    run_1859_short_chain();
    run_1859_deep_chain();
    run_1482_bindings_empty();
    run_1482_binding_index_untouched();
    run_1482_symid_iter_canonical();
    run_1482_pool_resolves_names();
    run_1482_no_pool_fallback();
    run_1482_alloc_env_frame_from_env();
    run_1482_desync_counter();
    run_1482_perf_microbench();
    run_1868_source_contract();
    run_1868_sequential_view();
    run_1868_remake();
    run_1869_source();
    run_1869_normal_chain();
    run_1869_cyclic_parent();
    std::println("\n=== Env batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
