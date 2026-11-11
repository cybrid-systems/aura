// @category: unit
// @reason: Issue #4543 — string_heap_ growth only through the locked mint API
//
// #2651 requires every Evaluator::string_heap_ growth to be serialized behind
// alloc_storage_lock_. ~650 primitive sites minted with an unlocked
// `idx = heap.size(); heap.push_back(s);` pair (#4519 AC2a TSan: unlocked
// writers racing locked pushers under fiber:spawn sharing → free(): invalid
// pointer / 0xbe-fill teardown SEGV). The heap is now a LockedStringHeap.
//
//   AC1: compile-time — LockedStringHeap has no push_back / emplace_back /
//        insert; Evaluator::string_heap_mut() hands out that type, so an
//        unlocked mint does not compile.
//   AC2: N threads mint into one bound heap: indices are unique and dense,
//        every slot holds the string its minter wrote, and the mint hook ran
//        once per index under the lock (TSan-clean).
//   AC3: N threads call string-minting primitives from swept files on one
//        shared Evaluator: every result string is intact and the indices are
//        pairwise distinct (the old size()+push_back pair handed two callers
//        the same slot). Run under --sanitizer=tsan for the race face.
//   AC4: the #4428 tenant stamp is written for primitive mints (the hook
//        replaces the per-site note_inrange_string_mint calls).
//   AC5: Issue #4548 code-point index — code_point_count / code_point_at /
//        code_point_slice agree with the #4472 reference walk on ASCII,
//        multi-byte and ill-formed strings above and below the threshold;
//        note_write, resize and a reallocating rewrite invalidate it.
//   AC6: Issue #5146 content index — find_content hits the earliest live
//        slot, note_write / release / replace / clear keep it honest, N
//        distinct intern-or-mint steps are not a heap scan, and repeated
//        string-append reuses one slot.

#include "test_harness.hpp"
#include "compiler/locked_string_heap.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <print>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::LockedStringHeap;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_string;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;
using aura::test::g_failed;
using aura::test::g_passed;

// ── AC1: the unlocked growth surface does not exist ──
template <class H>
concept HasPushBack = requires(H& h) { h.push_back(std::string{}); };
template <class H>
concept HasEmplaceBack = requires(H& h) { h.emplace_back("x"); };
template <class H>
concept HasInsert = requires(H& h) { h.insert(h.end(), std::string{}); };
template <class H>
concept HasMint = requires(H& h) {
    { h.mint(std::string{}) } -> std::same_as<std::uint64_t>;
};
static_assert(!HasPushBack<LockedStringHeap>, "#4543: no unlocked push_back");
static_assert(!HasEmplaceBack<LockedStringHeap>, "#4543: no unlocked emplace_back");
static_assert(!HasInsert<LockedStringHeap>, "#4543: no unlocked insert");
static_assert(HasMint<LockedStringHeap>, "#4543: mint is the growth API");
static_assert(!std::is_copy_constructible_v<LockedStringHeap>);
static_assert(
    std::is_same_v<std::remove_cvref_t<
                       decltype(std::declval<CompilerService&>().evaluator().string_heap_mut())>,
                   LockedStringHeap>,
    "#4543: Evaluator hands out the locked heap");

struct HookLog {
    std::vector<std::uint64_t> seen; // written only under the heap lock
};

void ac2_concurrent_mint() {
    std::println("\n--- AC2: concurrent mint into one bound heap ---");
    std::pmr::monotonic_buffer_resource res;
    std::recursive_mutex mu;
    LockedStringHeap heap(&res, &mu);
    HookLog log;
    heap.set_mint_hook(
        [](void* ctx, std::size_t idx) noexcept {
            static_cast<HookLog*>(ctx)->seen.push_back(idx);
        },
        &log);
    constexpr int kThreads = 8;
    constexpr int kPer = 4000;
    std::vector<std::vector<std::pair<std::uint64_t, std::string>>> got(kThreads);
    std::atomic<int> go{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            while (go.load(std::memory_order_acquire) == 0) {
            }
            got[t].reserve(kPer);
            for (int i = 0; i < kPer; ++i) {
                std::string s = "t" + std::to_string(t) + ":" + std::to_string(i) +
                                std::string(static_cast<std::size_t>(i % 40), 'x');
                const auto idx = heap.mint(s);
                got[t].emplace_back(idx, std::move(s));
            }
        });
    }
    go.store(1, std::memory_order_release);
    for (auto& th : ts)
        th.join();
    const std::size_t total = static_cast<std::size_t>(kThreads) * kPer;
    CHECK(heap.size() == total, "AC2: heap size == total mints");
    std::vector<char> hit(total, 0);
    bool unique = true, intact = true;
    for (const auto& per : got) {
        for (const auto& [idx, s] : per) {
            if (idx >= total || hit[idx]) {
                unique = false;
                continue;
            }
            hit[idx] = 1;
            if (heap[idx] != s)
                intact = false;
        }
    }
    CHECK(unique, "AC2: every minted index is unique and in range");
    CHECK(intact, "AC2: every slot holds its minter's string");
    CHECK(log.seen.size() == total, "AC2: hook ran once per mint");
    bool ordered = true;
    for (std::size_t i = 0; i < log.seen.size(); ++i)
        if (log.seen[i] != i)
            ordered = false;
    CHECK(ordered, "AC2: hook saw indices in mint order (ran inside the critical section)");
}

void ac3_shared_evaluator_prims() {
    std::println("\n--- AC3: string-minting prims on one shared Evaluator ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // obs_eval (panic-safe-source, the largest swept file) and pair
    // (number->string). Neither touches pairs_, so concurrent calls
    // exercise the string heap only. (query:* stats are not in the
    // primitive registry, so they are not reachable via lookup.)
    auto pss = ev.primitives().lookup("panic-safe-source");
    auto n2s = ev.primitives().lookup("number->string");
    CHECK(pss.has_value() && n2s.has_value(), "AC3: prims registered");
    if (!pss || !n2s)
        return;
    constexpr int kThreads = 6;
    constexpr int kPer = 1500;
    const std::size_t heap0 = ev.string_heap_size();
    std::vector<std::vector<std::uint64_t>> idxs(kThreads);
    std::atomic<int> bad{0};
    std::atomic<int> go{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            while (go.load(std::memory_order_acquire) == 0) {
            }
            idxs[t].reserve(kPer);
            for (int i = 0; i < kPer; ++i) {
                const EvalValue argv[1] = {make_int(i)};
                auto r =
                    ((t + i) % 2 == 0) ? (*pss)({}) : (*n2s)(std::span<const EvalValue>(argv, 1));
                if (!is_string(r)) {
                    bad.fetch_add(1);
                    continue;
                }
                idxs[t].push_back(as_string_idx(r));
            }
        });
    }
    go.store(1, std::memory_order_release);
    for (auto& th : ts)
        th.join();
    CHECK(bad.load() == 0, "AC3: every call returned a string");
    std::set<std::uint64_t> all;
    std::size_t n = 0;
    for (const auto& v : idxs) {
        for (auto i : v) {
            all.insert(i);
            ++n;
        }
    }
    CHECK(all.size() == n, "AC3: no two callers were handed the same heap slot");
    CHECK(ev.string_heap_size() == heap0 + n, "AC3: heap grew by exactly one slot per call");
    bool in_range = true;
    for (auto i : all)
        if (i < heap0 || i >= ev.string_heap_size())
            in_range = false;
    CHECK(in_range, "AC3: every index lands in the newly minted range");
}

void ac4_tenant_stamp_on_prim_mint() {
    std::println("\n--- AC4: #4428 stamp written for a primitive mint ---");

    // Issue #4747: the tenant-stamp arrays are only written when something can
    // READ them (effect sandbox on the production face, strict, or a
    // multi-tenant env). Arm the face so this row exercises the stamping path;
    // the unobservable case is pinned separately below.
    const bool was_active = aura::core::sandbox::is_sandbox_active();
    const bool was_strict = aura::core::sandbox::is_strict();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto pss = ev.primitives().lookup("panic-safe-source");
    CHECK(pss.has_value(), "AC4: panic-safe-source registered");
    if (!pss)
        return;
    auto r = (*pss)({});
    CHECK(is_string(r), "AC4: returns a string");
    if (!is_string(r))
        return;
    const auto idx = as_string_idx(r);
    CHECK(ev.string_slot_tenant_record_for_test(idx) == ev.capability_tenant_id() + 1,
          "AC4: minted slot carries the minting tenant's stamp");

    // Issue #4747 negative row: with nothing able to observe the stamp the mint
    // path deliberately skips the write (that is what removed the realloc race).
    aura::core::sandbox::set_mode(was_strict
                                      ? aura::core::sandbox::SandboxMode::Strict
                                      : (was_active ? aura::core::sandbox::SandboxMode::Restricted
                                                    : aura::core::sandbox::SandboxMode::Off));
    if (!aura::core::sandbox::is_sandbox_active() && ev.tenant_stamps_observable() == false) {
        auto r2 = (*pss)({});
        CHECK(is_string(r2), "AC4: unobservable-face mint still returns a string");
        if (is_string(r2))
            CHECK(ev.string_slot_tenant_record_for_test(as_string_idx(r2)) == 0,
                  "AC4: unobservable face leaves the slot unstamped (0 = owner-unknown, fail "
                  "closed)");
    }
}

// Reference walk (#4472) the cached index must agree with.
std::string ref_slice(const std::string& s, std::size_t a, std::size_t b) {
    const auto sb = aura::compiler::utf8_byte_offset(s, a);
    const auto eb = aura::compiler::utf8_byte_offset(s, b);
    return sb >= eb ? std::string{} : s.substr(sb, eb - sb);
}

bool agrees(const LockedStringHeap& h, std::size_t idx, const std::string& s) {
    const auto n = aura::compiler::utf8_unit_count(s);
    if (h.code_point_count(idx) != n)
        return false;
    for (std::size_t i = 0; i <= n + 2; ++i) {
        if (h.code_point_at(idx, i) != aura::compiler::utf8_code_point_at(s, i))
            return false;
    }
    std::string out;
    const std::size_t step = n > 300 ? 37 : 1;
    for (std::size_t a = 0; a <= n + 1; a += step) {
        for (std::size_t b : {a, a + 1, a + 63, a + 64, a + 65, n, n + 5}) {
            if (!h.code_point_slice(idx, a, b, out) || out != ref_slice(s, a, b))
                return false;
        }
    }
    return true;
}

void ac5_code_point_index() {
    std::println("\n--- AC5: #4548 cached code-point index ---");
    LockedStringHeap h; // unbound: unlocked
    std::string ascii(5000, 'a');
    for (std::size_t i = 0; i < ascii.size(); ++i)
        ascii[i] = static_cast<char>('a' + (i * 7) % 26);
    std::string mixed;
    for (int i = 0; i < 1500; ++i) {
        mixed += (i % 3 == 0) ? "\xC3\xA9" : (i % 5 == 0 ? "\xE4\xB8\xAD" : "x");
        if (i % 97 == 0)
            mixed += "\xF0\x9F\x98\x80"; // 4-byte
        if (i % 211 == 0)
            mixed += "\xFF"; // ill-formed: one unit
    }
    const std::string small = "h\xC3\xA9llo";
    const auto ia = h.mint(ascii);
    const auto im = h.mint(mixed);
    const auto is = h.mint(small);
    CHECK(agrees(h, ia, ascii), "AC5: ASCII long string agrees with the reference walk");
    CHECK(agrees(h, im, mixed), "AC5: multi-byte / ill-formed string agrees");
    CHECK(agrees(h, is, small), "AC5: short string (below threshold) agrees");
    CHECK(h.cp_index_entries() == 2, "AC5: index built only for the two long strings");
    CHECK(h.code_point_at(99, 0) == -1 && h.code_point_count(99) == LockedStringHeap::npos,
          "AC5: out-of-range slot");

    // Same-size in-place write that makes the ASCII string multi-byte.
    {
        auto& s = h[ia];
        s[10] = '\xC3';
        s[11] = '\xA9';
        h.note_write(ia);
        CHECK(agrees(h, ia, h[ia]), "AC5: note_write after a same-size byte write re-indexes");
        CHECK(h.code_point_count(ia) == ascii.size() - 1, "AC5: two bytes became one code point");
    }
    // A rewrite that changes length/pointer is caught without note_write.
    {
        h[im] = std::string(4000, 'z');
        CHECK(agrees(h, im, h[im]), "AC5: rewritten slot (new size) re-indexes");
    }
    // Structural writes move the generation.
    {
        const auto g0 = h.write_generation();
        h.resize(2);
        CHECK(h.write_generation() != g0, "AC5: resize moves the write generation");
        const auto i2 = h.mint(mixed);
        CHECK(i2 == 2 && agrees(h, i2, mixed), "AC5: reused index after resize re-indexes");
    }
}

void ac6_content_index_5146() {
    std::println("\n--- AC6: #5146 content index ---");
    LockedStringHeap h;
    const auto first = h.mint(std::string("zz"));
    const auto second = h.mint(std::string("zz"));
    CHECK(first != second, "5146: mint itself still appends a slot");
    CHECK(h.find_content("zz") == first, "5146: earliest equal slot is the intern hit");
    CHECK(h.find_content("nope") == LockedStringHeap::npos, "5146: a miss is npos");

    h[first] = "yy";
    h.note_write(first);
    CHECK(h.find_content("yy") == first, "5146: note_write indexes the new content");
    CHECK(h.find_content("zz") == second, "5146: note_write drops the old content");

    std::vector<std::uint8_t> seen(h.size(), 1);
    seen[second] = 0;
    std::vector<std::uint8_t> free_at(h.size(), 0);
    h.release_unmarked(seen, free_at);
    CHECK(h.find_content("zz") == LockedStringHeap::npos, "5146: a released slot leaves the index");
    CHECK(h.find_content("yy") == first, "5146: a live slot stays indexed across a release");

    const auto again = h.mint(std::string("zz"));
    CHECK(h.find_content("zz") == again, "5146: a reused slot is indexed as its new content");
    CHECK(h[again] == "zz", "5146: the reused slot holds the new bytes");

    LockedStringHeap hr;
    (void)hr.mint(std::string("p"));
    (void)hr.mint(std::string("q"));
    hr.resize(1);
    CHECK(hr.find_content("p") == 0, "5146: resize keeps the prefix indexed");
    CHECK(hr.find_content("q") == LockedStringHeap::npos, "5146: resize drops the tail");

    std::pmr::vector<std::string> rebuilt;
    rebuilt.emplace_back("aa");
    rebuilt.emplace_back("bb");
    h.replace(std::move(rebuilt));
    CHECK(h.find_content("aa") == 0 && h.find_content("bb") == 1,
          "5146: replace rebuilds the index");
    CHECK(h.find_content("zz") == LockedStringHeap::npos, "5146: replace drops the old index");
    h.clear();
    CHECK(h.empty() && h.find_content("aa") == LockedStringHeap::npos,
          "5146: clear drops the index");

    // N distinct intern-or-mint steps. A heap scan is quadratic; the bound
    // is loose enough for a busy runner and tight enough to catch the scan.
    constexpr int kN = 15000;
    LockedStringHeap heap;
    int false_hits = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) {
        const auto s = "s" + std::to_string(i);
        if (heap.find_content(s) != LockedStringHeap::npos)
            ++false_hits;
        (void)heap.mint(s);
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    std::println("  5146 distinct {} intern-or-mint: {} ms", kN, ms);
    CHECK(false_hits == 0, "5146: a distinct string misses");
    CHECK(heap.size() == static_cast<std::size_t>(kN), "5146: one slot per distinct string");
    CHECK(ms < 3000, "5146: distinct interns are not a heap scan");
    bool hits = true;
    for (int i = 0; i < kN; i += 97) {
        if (heap.find_content("s" + std::to_string(i)) == LockedStringHeap::npos)
            hits = false;
    }
    CHECK(hits, "5146: indexed strings are found");

    CompilerService cs;
    auto& ev = cs.evaluator();
    auto append = ev.primitives().lookup("string-append");
    CHECK(append.has_value(), "5146: string-append registered");
    if (!append)
        return;
    const EvalValue a = make_string(static_cast<std::uint64_t>(ev.push_string_heap("same")));
    const EvalValue b = make_string(static_cast<std::uint64_t>(ev.push_string_heap("pieces")));
    const EvalValue args[2] = {a, b};
    const auto before = ev.string_heap_size();
    EvalValue got{};
    for (int i = 0; i < 200; ++i)
        got = (*append)(std::span<const EvalValue>(args, 2));
    CHECK(is_string(got), "5146: string-append returns a string");
    CHECK(ev.string_heap_size() == before + 1, "5146: repeated append reuses one slot");
    if (is_string(got)) {
        const auto again_s = (*append)(std::span<const EvalValue>(args, 2));
        CHECK(is_string(again_s) && as_string_idx(again_s) == as_string_idx(got),
              "5146: repeated append returns the same slot");
    }
}

} // namespace

int main() {
    std::println("=== test_locked_string_heap (#4543) ===");
    ac2_concurrent_mint();
    ac3_shared_evaluator_prims();
    ac4_tenant_stamp_on_prim_mint();
    ac5_code_point_index();
    ac6_content_index_5146();
    std::println("\n=== test_locked_string_heap: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
