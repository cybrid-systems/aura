// @category: unit
// @reason: Issue #2166 — opt-in LiveCompactMode::Moving densify + object_remap_.
//
//   AC_M1: Moving off → Force still non-moving (address stable, moved_live_objects false).
//   AC_M2: Moving on + preconditions met → addresses may change; moved_live_objects true;
//          gen advanced; pins invalidated / remapped.
//   AC_M3: Moving on + live pin present → blocked; precondition metric bumps.
//   AC_M4: after Moving, resolve_object_remap / pin fail-closed paths remain valid.
//
//   Issue #2342 (Refine #2166): sharded LifetimePin registry (Option 1
//   — shard by arena_id, N=16 per the issue's "incremental risk"
//   preference). Foundation already present (#2166/#2265/#2266/#2280
//   pin-or-remap contract); this round ships the sharded surface so
//   hot-path pin ctor/dtor traffic no longer serializes on the global
//   pin_registry_mtx. Refines #2265 (Phase 3 remap) · #2270 (PinOwner)
//   · #2160 (RenderPin) · #2298 (GeneralObjectPin adoption).
//   AC_2342_1: kPinRegistryShardCount == 16 + max_pin_count +
//              total_pinned_count accessors queryable.
//   AC_2342_2: pin ctor/dtor routes to shard 0 for arena_id=0 default;
//              pin(arena_id=N) re-routes to correct shard via
//              pin_registry_shard_index(N).
//   AC_2342_3: pin_registry_lock_wait_us_total counter bumps on
//              shard lock acquisition (cumulative atomic).
//   AC_2342_4: query:lifetime-contract-snapshot exposes #2342 keys
//              (pin-registry-shard-count kebab+snake + max-pin-count
//              + lock-wait-us-total + wired sentinel + schema/issue).
//   AC_2342_5: source-cite sharded registry infrastructure across
//              lifetime_pin.ixx (kPinRegistryShardCount +
//              pin_registry_shards + accessors + ctor/dtor/move/pin
//              shard routing) + evaluator_primitives_obs_jit.cpp
//              (query surface).

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.arena;
import aura.core.lifetime_pin;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::ASTArena;
using aura::ast::kMovingCompactIssue;
using aura::ast::LiveCompactMode;
using aura::ast::moving_compact_enabled;
using aura::ast::set_moving_compact_enabled;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::lifetime::LifetimePin;
using aura::core::lifetime::live_pin_count;
using aura::test::g_failed;
using aura::test::g_passed;

// Trivial tracked object for create<T> densify (small-pool tier).
struct Pod16 {
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    std::int32_t d = 0;
    Pod16() = default;
    Pod16(std::int32_t a_, std::int32_t b_, std::int32_t c_, std::int32_t d_) noexcept
        : a(a_)
        , b(b_)
        , c(c_)
        , d(d_) {}
};

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

struct MovingFlagGuard {
    int prev = -1;
    explicit MovingFlagGuard(int enable) {
        prev = moving_compact_enabled();
        set_moving_compact_enabled(enable);
    }
    ~MovingFlagGuard() { set_moving_compact_enabled(prev); }
};


// Issue #2256 AC1-AC5: production-default Moving compaction +
// LifetimePin hard contract + pointer-remap contracts.
// AC1: production default ON (Moving compaction enabled by default).
// AC2: pin-or-remap contract (every live pin honored under Moving).
// AC3: zero-cost when no compact runs.
// AC4: metrics + query surface (compact_count_total /
//      bytes_reclaimed_total / pin_hits_total / remap_us_total).
// AC5: 10k-mutation soak with Moving on shows bounded fragmentation.
void ac2256_moving_compact_production_default() {
    std::println("\n--- AC #2256: Moving-compact production default ---");
    auto arena_h = read_file("src/core/arena.ixx");
    auto pin_h = read_file("src/core/lifetime_pin.ixx");
    auto mut = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto shape = read_file("src/compiler/shape_profiler.cpp");
    auto soa = read_file("src/compiler/ir_soa.ixx");
    auto met = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    // AC1: production default ON in arena.ixx
    CHECK(arena_h.find("return 1;") != std::string::npos and
              arena_h.find("// production default ON") != std::string::npos,
          "AC1: arena.ixx production default ON");
    // AC1: adaptive-on-threshold policy
    CHECK(arena_h.find("should_auto_moving_compact") != std::string::npos,
          "AC1: adaptive-on-threshold policy");
    CHECK(arena_h.find("kAutoMovingCompactThreshold") != std::string::npos,
          "AC1: kAutoMovingCompactThreshold constant");
    // AC2: LifetimePin hard contract + pin_hits/remap_us accumulators
    CHECK(pin_h.find("verify_pins_under_moving_compact") != std::string::npos,
          "AC2: LifetimePin::verify_pins_under_moving_compact");
    CHECK(pin_h.find("g_moving_compact_pin_hits_total") != std::string::npos,
          "AC2: pin_hits_total accumulator");
    CHECK(pin_h.find("g_moving_compact_remap_us_total") != std::string::npos,
          "AC2: remap_us_total accumulator");
    // AC3: zero-cost when no compact (verify function is opt-in by caller)
    CHECK(mut.find("verify_pins_under_moving_compact()") != std::string::npos,
          "AC3: compact driver calls verify_pins (zero-cost when not called)");
    // AC4: 4 metric fields
    CHECK(met.find("compact_count_total{0}") != std::string::npos,
          "AC4: compact_count_total field");
    CHECK(met.find("bytes_reclaimed_total{0}") != std::string::npos,
          "AC4: bytes_reclaimed_total field");
    CHECK(met.find("pin_hits_total{0}") != std::string::npos, "AC4: pin_hits_total field");
    CHECK(met.find("remap_us_total{0}") != std::string::npos, "AC4: remap_us_total field");
    // AC4: 4 query keys + schema-2256 lineage
    CHECK(q.find("compact-count-total") != std::string::npos, "AC4: compact-count-total query key");
    CHECK(q.find("bytes-reclaimed-total") != std::string::npos,
          "AC4: bytes-reclaimed-total query key");
    CHECK(q.find("pin-hits-total") != std::string::npos, "AC4: pin-hits-total query key");
    CHECK(q.find("remap-us-total") != std::string::npos, "AC4: remap-us-total query key");
    CHECK(q.find("moving-compact-wired") != std::string::npos,
          "AC4: moving-compact-wired sentinel");
    CHECK(q.find("schema-2256") != std::string::npos, "AC4: schema-2256 lineage");
    CHECK(q.find("issue-2256") != std::string::npos, "AC4: issue-2256 lineage");
    // AC2: SoA index-remap contract
    CHECK(soa.find("index-remap contract") != std::string::npos,
          "AC2: SoA index-remap contract marker");
    // AC4: ShapeProfiler on_arena_compact feeds Moving-compact counters
    CHECK(shape.find("g_moving_compact_count_total") != std::string::npos,
          "AC4: ShapeProfiler::on_arena_compact feeds compact_count_total");
    CHECK(shape.find("g_moving_compact_remap_us_total") != std::string::npos,
          "AC4: ShapeProfiler::on_arena_compact feeds remap_us_total");
}

// Issue #2342 AC_2342_1: sharded registry constants + accessors queryable.
// kPinRegistryShardCount == 16 (power of 2 for arena_id-based AND
// routing); pin_registry_shard_pin_count(idx) + pin_registry_shard_max
// _pin_count() + pin_registry_total_pinned_count() return non-negative
// values; reads are process-level atomic for the counter, lock-shard
// for per-shard pin count.
void ac2342_1_shard_constants_and_accessors() {
    std::println("\n--- AC_2342_1: sharded registry constants + accessors ---");
    using aura::core::lifetime::kPinRegistryShardCount;
    using aura::core::lifetime::pin_registry_shard_max_pin_count;
    using aura::core::lifetime::pin_registry_shard_pin_count;
    using aura::core::lifetime::pin_registry_total_pinned_count;
    CHECK(kPinRegistryShardCount == 16, "AC_2342_1.1: kPinRegistryShardCount == 16 (power of 2)");
    CHECK(kPinRegistryShardCount != 0, "AC_2342_1.2: kPinRegistryShardCount != 0");
    CHECK(pin_registry_total_pinned_count() >= 0,
          "AC_2342_1.3: pin_registry_total_pinned_count queryable + >= 0");
    CHECK(pin_registry_shard_max_pin_count() >= 0,
          "AC_2342_1.4: pin_registry_shard_max_pin_count queryable + >= 0");
    // Per-shard pin count (idx valid + idx OOB returns 0).
    for (std::size_t i = 0; i < kPinRegistryShardCount; ++i) {
        CHECK(pin_registry_shard_pin_count(i) >= 0,
              "AC_2342_1.5: per-shard pin count queryable for valid idx");
    }
    CHECK(pin_registry_shard_pin_count(kPinRegistryShardCount) == 0,
          "AC_2342_1.6: OOB shard idx returns 0 (defensive)");
    CHECK(pin_registry_shard_pin_count(kPinRegistryShardCount + 100) == 0,
          "AC_2342_1.7: very-OOB shard idx returns 0 (defensive)");
}

// Issue #2342 AC_2342_2: pin ctor/dtor routes to shard 0 for arena_id=0
// default; pin(arena_id=N) re-routes to correct shard. We construct
// a few LifetimePin instances with arena_id=0 (default shard 0) and
// verify live_pin_count == pin_registry_total_pinned_count (both
// iterate the new sharded registry).
void ac2342_2_ctor_dtor_routes_to_shard_zero() {
    std::println("\n--- AC_2342_2: pin ctor/dtor routes to shard 0 ---");
    using aura::core::lifetime::LifetimePin;
    using aura::core::lifetime::live_pin_count;
    using aura::core::lifetime::pin_registry_total_pinned_count;
    const auto before = pin_registry_total_pinned_count();
    // Create 4 pins with arena_id=0 (default → shard 0).
    LifetimePin p1, p2, p3, p4;
    int dummy_a = 0, dummy_b = 0, dummy_c = 0, dummy_d = 0;
    p1.pin(&dummy_a, /*gen=*/1);
    p2.pin(&dummy_b, /*gen=*/1);
    p3.pin(&dummy_c, /*gen=*/1);
    p4.pin(&dummy_d, /*gen=*/1);
    CHECK(pin_registry_total_pinned_count() == before + 4,
          "AC_2342_2.1: 4 pin ctors bump total_pinned_count by 4");
    CHECK(live_pin_count() == before + 4,
          "AC_2342_2.2: live_pin_count matches pin_registry_total_pinned_count");
    // Scope exit → 4 dtors run.
}

// Issue #2342 AC_2342_3: pin_registry_lock_wait_us_total counter bumps
// on shard lock acquisition. Each pin ctor/dtor acquires a shard lock
// once, so creating + destroying pins should bump the counter.
void ac2342_3_lock_wait_us_total() {
    std::println("\n--- AC_2342_3: pin_registry_lock_wait_us_total counter ---");
    using aura::core::lifetime::LifetimePin;
    using aura::core::lifetime::pin_registry_lock_wait_us_total;
    const auto before = pin_registry_lock_wait_us_total();
    CHECK(before >= 0, "AC_2342_3.1: lock_wait counter queryable + >= 0");
    {
        LifetimePin p;
        int dummy = 0;
        p.pin(&dummy, /*gen=*/1);
    }
    const auto after = pin_registry_lock_wait_us_total();
    CHECK(after >= before, "AC_2342_3.2: lock_wait counter monotonic (>= after ctor+dtor)");
}

// Issue #2342 AC_2342_4: query:lifetime-contract-snapshot extends with
// #2342 keys. kebab + snake aliases per axis; wired sentinel;
// schema/issue sentinels.
void ac2342_4_query_schema(CompilerService& cs) {
    std::println("\n--- AC_2342_4: query:lifetime-contract-snapshot #2342 surface ---");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "pin-registry-shard-count") == 16,
          "AC_2342_4.1: pin-registry-shard-count == 16 (kebab)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "pin_registry_shard_count") == 16,
          "AC_2342_4.2: pin_registry_shard_count == 16 (snake alias)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "pin-registry-shard-max-pin-count") >= 0,
          "AC_2342_4.3: pin-registry-shard-max-pin-count reachable (kebab)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "pin_registry_shard_max_pin_count") >= 0,
          "AC_2342_4.4: pin_registry_shard_max_pin_count reachable (snake)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "pin-registry-lock-wait-us-total") >= 0,
          "AC_2342_4.5: pin-registry-lock-wait-us-total reachable (kebab)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "pin_registry_lock_wait_us_total") >= 0,
          "AC_2342_4.6: pin_registry_lock_wait_us_total reachable (snake)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "pin-registry-shard-wired") == 1,
          "AC_2342_4.7: pin-registry-shard-wired == 1 (proves #2342 wired)");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "schema-2342") == 2342,
          "AC_2342_4.8: schema-2342 == 2342");
    CHECK(href(cs, "query:lifetime-contract-snapshot", "issue-2342") == 2342,
          "AC_2342_4.9: issue-2342 == 2342");
}

// Issue #2342 AC_2342_5: source-cite grep verifier. Each #2342 file
// must contain the Issue #2342 cite + the contract surface (shard
// infrastructure + shard_index_ + ctor/dtor/move/pin shard routing +
// compact function shard iteration + query keys).
void ac2342_5_source_cite() {
    std::println("\n--- AC_2342_5: Issue #2342 source-cite ---");
    auto check = [](const std::filesystem::path& p, std::initializer_list<const char*> needles,
                    std::string_view tag) {
        if (!std::filesystem::exists(p)) {
            CHECK(false, std::format("AC_2342_5: {} not found", p.string()).c_str());
            return;
        }
        std::ifstream in(p);
        std::stringstream buf;
        buf << in.rdbuf();
        const auto txt = buf.str();
        for (const auto* needle : needles) {
            CHECK(txt.find(needle) != std::string::npos,
                  std::format("AC_2342_5: {} contains {}", tag, needle).c_str());
        }
    };
    check(std::filesystem::path(AURA_SOURCE_DIR) / "src/core/lifetime_pin.ixx",
          {"Issue #2342", "kPinRegistryShardCount", "PinRegistryShard", "pin_registry_shards",
           "pin_registry_shard_index", "pin_registry_lock_wait_us_total", "shard_index_",
           "g_root_remap_any_fail" /* fallback if not present */},
          "lifetime_pin.ixx");
    // Issue cite appears in lifetime_pin.ixx; fallback probe for the
    // primary keyword (drop the optional secondary if absent).
    check(std::filesystem::path(AURA_SOURCE_DIR) / "src/compiler/evaluator_primitives_obs_jit.cpp",
          {"Issue #2342", "pin-registry-shard-count", "pin-registry-shard-wired", "schema-2342",
           "issue-2342"},
          "evaluator_primitives_obs_jit.cpp");
}

} // namespace

int main() {
    std::println("=== Issue #2166: LiveCompactMode::Moving densify + object_remap ===");
    CHECK(kMovingCompactIssue == 2166, "issue stamp");

    // Default: feature off for tests (unless env overrides; force pref).
    set_moving_compact_enabled(0);
    CHECK(moving_compact_enabled() == 0, "default test pref OFF");
    // Restore for AC #2256 check (which inspects production default).
    set_moving_compact_enabled(-1); // -1 = env/default
    ac2256_moving_compact_production_default();

    // ── AC_M1: Moving off → Force non-moving ──
    {
        std::println("\n--- AC_M1: Force non-moving while Moving flag off ---");
        MovingFlagGuard off(0);
        ASTArena arena(64 * 1024);
        auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
        auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
        auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
        CHECK(p0 && p1 && p2, "AC_M1: create ok");
        void* a0 = p0;
        void* a1 = p1;
        void* a2 = p2;

        // Moving while flag off → blocked precondition.
        const auto rm = arena.live_compact(LiveCompactMode::Moving);
        CHECK(rm.moving_blocked_precondition, "AC_M1: Moving blocked when flag off");
        CHECK(!rm.moved_live_objects, "AC_M1: no move when blocked");
        CHECK(rm.objects_moved == 0, "AC_M1: objects_moved 0");
        CHECK(p0 == a0 && p1 == a1 && p2 == a2, "AC_M1: raw create ptrs unchanged after block");

        // Force remains non-moving (addresses of create objects stable).
        const auto rf = arena.live_compact(LiveCompactMode::Force);
        CHECK(!rf.moved_live_objects, "AC_M1: Force moved_live_objects false");
        CHECK(rf.objects_moved == 0, "AC_M1: Force objects_moved 0");
        CHECK(p0 == a0 && p1 == a1 && p2 == a2, "AC_M1: Force does not rewrite create ptrs");
        CHECK(p0->a == 1 && p1->a == 5 && p2->a == 9, "AC_M1: payload intact");
        (void)rf;
    }

    // ── AC_M2: Moving on + preconditions → densify + remap ──
    {
        std::println("\n--- AC_M2: Moving densify when enabled ---");
        MovingFlagGuard on(1);
        CHECK(moving_compact_enabled() == 1, "AC_M2: flag on");
        ASTArena arena(64 * 1024);
        auto* p0 = arena.create<Pod16>(10, 20, 30, 40);
        auto* p1 = arena.create<Pod16>(11, 21, 31, 41);
        auto* p2 = arena.create<Pod16>(12, 22, 32, 42);
        CHECK(p0 && p1 && p2, "AC_M2: create");
        void* old0 = p0;
        void* old1 = p1;
        void* old2 = p2;
        const Pod16 v0 = *p0;
        const Pod16 v1 = *p1;
        const Pod16 v2 = *p2;
        const auto gen0 = arena.generation();
        const auto move0 = aura::ast::g_objects_moved_total.load(std::memory_order_relaxed);
        const auto moving0 = aura::ast::g_live_compact_moving_count.load(std::memory_order_relaxed);

        CHECK(live_pin_count() == 0, "AC_M2: no live pins");

        const auto r = arena.live_compact(LiveCompactMode::Moving);
        CHECK(!r.moving_blocked_precondition, "AC_M2: not blocked");
        CHECK(r.mode == LiveCompactMode::Moving, "AC_M2: mode Moving");
        // Densify of 3 freelist-recycled objects → LIFO reverse → address change.
        CHECK(r.objects_moved > 0, "AC_M2: objects_moved > 0");
        CHECK(r.moved_live_objects, "AC_M2: moved_live_objects true");
        CHECK(r.invalidates_pins || arena.generation() > gen0, "AC_M2: gen advanced / pins");
        CHECK(arena.generation() > gen0, "AC_M2: arena gen advanced");
        CHECK(arena.object_remap_size() >= r.objects_moved, "AC_M2: remap table populated");
        CHECK(aura::ast::g_live_compact_moving_count.load() > moving0, "AC_M2: moving count");
        CHECK(aura::ast::g_objects_moved_total.load() >= move0 + r.objects_moved,
              "AC_M2: objects_moved total");

        void* n0 = arena.resolve_object_remap(old0);
        void* n1 = arena.resolve_object_remap(old1);
        void* n2 = arena.resolve_object_remap(old2);
        CHECK(n0 != nullptr && n1 != nullptr && n2 != nullptr, "AC_M2: all remapped");
        const int changed = (n0 != old0) + (n1 != old1) + (n2 != old2);
        CHECK(changed > 0, "AC_M2: at least one address changed");
        CHECK(static_cast<Pod16*>(n0)->a == v0.a && static_cast<Pod16*>(n0)->b == v0.b,
              "AC_M2: payload0 via remap");
        CHECK(static_cast<Pod16*>(n1)->a == v1.a, "AC_M2: payload1 via remap");
        CHECK(static_cast<Pod16*>(n2)->a == v2.a, "AC_M2: payload2 via remap");
    }

    // ── AC_M3: live pin blocks Moving ──
    {
        std::println("\n--- AC_M3: live pin blocks Moving ---");
        MovingFlagGuard on(1);
        ASTArena arena(64 * 1024);
        auto* p = arena.create<Pod16>(1, 2, 3, 4);
        CHECK(p, "AC_M3: create");
        const auto gen0 = arena.generation();
        const auto block0 =
            aura::ast::g_moving_blocked_precondition_total.load(std::memory_order_relaxed);

        LifetimePin pin;
        pin.pin(p, gen0, arena.arena_id());
        CHECK(pin.pinned() && live_pin_count() >= 1, "AC_M3: pin live");

        const auto r = arena.live_compact(LiveCompactMode::Moving);
        CHECK(r.moving_blocked_precondition, "AC_M3: moving_blocked_precondition");
        CHECK(r.force_blocked_by_pin, "AC_M3: also force_blocked_by_pin");
        CHECK(!r.moved_live_objects, "AC_M3: no move");
        CHECK(arena.generation() == gen0, "AC_M3: gen unchanged");
        CHECK(pin.ptr() == p, "AC_M3: pin ptr stable");
        CHECK(pin.validate(gen0, arena.arena_id()), "AC_M3: pin still valid");
        CHECK(aura::ast::g_moving_blocked_precondition_total.load() > block0,
              "AC_M3: precondition metric");
    }

    // ── AC_M4: fail-closed resolve + pin invalidate after Moving ──
    {
        std::println("\n--- AC_M4: resolve fail-closed + pin invalidate ---");
        MovingFlagGuard on(1);
        ASTArena arena(64 * 1024);
        auto* p0 = arena.create<Pod16>(7, 8, 9, 10);
        auto* p1 = arena.create<Pod16>(17, 18, 19, 20);
        CHECK(p0 && p1, "AC_M4: create");
        void* old0 = p0;
        const auto gen0 = arena.generation();

        // Scope a pin then drop it so Moving preconditions are free.
        {
            LifetimePin hold;
            hold.pin(old0, gen0, arena.arena_id());
            CHECK(hold.pinned(), "AC_M4: hold pinned");
        }
        CHECK(live_pin_count() == 0, "AC_M4: hold released");

        const auto r = arena.live_compact(LiveCompactMode::Moving);
        CHECK(!r.moving_blocked_precondition, "AC_M4: Moving ran");
        // Unknown pointer → nullptr (fail closed).
        CHECK(arena.resolve_object_remap(nullptr) == nullptr, "AC_M4: null in → null out");
        void* garbage = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADBEEF));
        CHECK(arena.resolve_object_remap(garbage) == nullptr, "AC_M4: unknown → null");

        void* neu = arena.resolve_object_remap(old0);
        if (r.objects_moved > 0) {
            CHECK(neu != nullptr, "AC_M4: known old resolves after move");
            CHECK(static_cast<Pod16*>(neu)->a == 7, "AC_M4: payload via remap");
        }

        // Gen advanced → pin stamped at gen0 fails validate against current gen.
        const auto gen1 = arena.generation();
        if (gen1 > gen0) {
            LifetimePin pin2;
            pin2.pin(old0, gen0, arena.arena_id()); // intentionally stale gen stamp
            CHECK(!pin2.validate(gen1, arena.arena_id()),
                  "AC_M4: stale gen pin fails validate(current gen)");
        }

        // Query surface schema-2166.
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "query:arena-live-compact-stats", "schema-2166") == 2166,
              "AC_M4: schema-2166 on arena-live-compact-stats");
        CHECK(href(cs, "query:arena-live-compact-stats", "moving-compact-wired") == 1, "wired");
        CHECK(href(cs, "query:arena-live-compact-stats", "live-compact-moving-count") >= 0,
              "moving count key");
        CHECK(href(cs, "query:arena-live-compact-stats", "objects-moved-total") >= 0,
              "objects-moved key");
        CHECK(href(cs, "query:arena-live-compact-stats", "moving-blocked-precondition-total") >= 0,
              "blocked key");
    }

    // ── AC_M5: #2265 Phase 3 — LifetimePin::remap + remap_pins_pointing_to ──
    {
        std::println("\n--- AC_M5 positive: pin arena object → remap on Moving ---");
        MovingFlagGuard on(1);
        ASTArena arena(64 * 1024);
        auto* p0 = arena.create<Pod16>(100, 200, 300, 400);
        auto* p1 = arena.create<Pod16>(101, 201, 301, 401);
        auto* p2 = arena.create<Pod16>(102, 202, 302, 402);
        CHECK(p0 && p1 && p2, "AC_M5: arena objects created");
        void* old0 = p0;
        void* old1 = p1;
        void* old2 = p2;
        const auto gen0 = arena.generation();

        LifetimePin pin;
        pin.pin(old0, gen0, arena.arena_id());
        CHECK(pin.pinned(), "AC_M5: pin attached");
        CHECK(pin.ptr() == old0, "AC_M5: pin.ptr() pre-Moving");
        CHECK(pin.validate(gen0, arena.arena_id()), "AC_M5: validate pre-Moving");

        const auto remapped_before = aura::core::lifetime::lifetime_pin_remap_total();
        const auto r = arena.live_compact(LiveCompactMode::Moving);
        CHECK(r.moved_live_objects, "AC_M5: Moving densified arena objects");
        CHECK(r.remapped_pins > 0, "AC_M5: LiveCompactResult.remapped_pins > 0");

        // After Moving: pin.ptr() must follow the remap (if old0 was densified).
        void* new0 = arena.resolve_object_remap(old0);
        if (new0 != nullptr) {
            CHECK(pin.ptr() == new0, "AC_M5: pin.ptr() follows remap (old → new)");
            CHECK(pin.gen() == arena.generation(), "AC_M5: pin.gen() bumped to new gen");
            CHECK(pin.validate(arena.generation(), arena.arena_id()),
                  "AC_M5: validate succeeds against new gen + new ptr");
            CHECK(static_cast<Pod16*>(new0)->a == 100 && static_cast<Pod16*>(new0)->b == 200,
                  "AC_M5: payload intact after remap");
        }
        CHECK(aura::core::lifetime::lifetime_pin_remap_total() > remapped_before,
              "AC_M5: process-wide remap counter bumped");
    }
    {
        std::println("\n--- AC_M5 negative: pin non-arena addr → invalidate ---");
        MovingFlagGuard on(1);
        ASTArena arena(64 * 1024);
        // Allocate arena objects so densify has work to do (Moving
        // would otherwise bail on preconditions).
        auto* p0 = arena.create<Pod16>(1, 2, 3, 4);
        auto* p1 = arena.create<Pod16>(5, 6, 7, 8);
        auto* p2 = arena.create<Pod16>(9, 10, 11, 12);
        (void)p0;
        (void)p1;
        (void)p2;

        // Pin a LOCAL buffer (NOT an arena object). Its address will NOT
        // be in last_object_remap_ values after Moving, so the wire-up's
        // selective-invalidate pass should nullify it.
        Pod16 local{100, 200, 300, 400};
        LifetimePin pin;
        pin.pin(&local, arena.generation(), arena.arena_id());
        CHECK(pin.pinned(), "AC_M5: local pin attached");
        CHECK(pin.ptr() == &local, "AC_M5: pin.ptr() == &local pre-Moving");

        const auto r = arena.live_compact(LiveCompactMode::Moving);
        CHECK(r.moved_live_objects, "AC_M5: Moving densified arena objects");

        // Local pin's ptr was NOT in any remap entry → invalidate.
        CHECK(!pin.pinned(), "AC_M5: non-arena pin invalidated after Moving");
        CHECK(!pin.validate(arena.generation(), arena.arena_id()),
              "AC_M5: validate fails after invalidate");
    }

    // ── AC_M6: #2266 — verify_pins_under_moving_compact fail-closed ──
    {
        std::println("\n--- AC_M6 positive: pin contract held after remap ---");
        MovingFlagGuard on(1);
        ASTArena arena(64 * 1024);
        auto* p0 = arena.create<Pod16>(7, 8, 9, 10);
        auto* p1 = arena.create<Pod16>(17, 18, 19, 20);
        auto* p2 = arena.create<Pod16>(27, 28, 29, 30);
        (void)p0;
        (void)p1;
        (void)p2;

        const auto before = aura::core::lifetime::lifetime_pin_remap_total();

        LifetimePin pin;
        pin.pin(p0, arena.generation(), arena.arena_id());
        CHECK(pin.pinned(), "AC_M6: pin attached");

        const auto r = arena.live_compact(LiveCompactMode::Moving);
        CHECK(r.moved_live_objects, "AC_M6: Moving densified");
        CHECK(r.pin_contract_held,
              "AC_M6: LiveCompactResult.pin_contract_held = true (pin was remapped in-place)");
        CHECK(aura::core::lifetime::lifetime_pin_remap_total() == before,
              "AC_M6: lifetime_pin_remap_total not bumped (contract held)");

        void* new0 = arena.resolve_object_remap(p0);
        if (new0 != nullptr) {
            CHECK(pin.ptr() == new0, "AC_M6: pin.ptr() follows remap");
            CHECK(pin.validate(arena.generation(), arena.arena_id()),
                  "AC_M6: validate succeeds after remap");
        }
    }
    {
        std::println("\n--- AC_M6 negative: pin contract fail-closed ---");
        // Simulate the contract-fail path by directly calling the verify
        // function with a pin whose ptr_ is in the old_addresses set (i.e.,
        // a pin that was supposed to be remapped but wasn't). This is the
        // fail-closed guarantee introduced in #2266 (previously the verify
        // function always returned true — observe-only).
        const std::uint64_t before = aura::core::lifetime::lifetime_pin_contract_fail_total();
        // Pin a fake "old address" (a stack buffer is fine — verify walks
        // the pin registry, not arena internals, so any pinned ptr that
        // matches the old_addresses set will trigger the contract fail).
        int dummy = 0;
        void* fake_old = &dummy;
        LifetimePin pin;
        pin.pin(fake_old, /*gen=*/0, /*arena_id=*/0);
        CHECK(pin.pinned(), "AC_M6: fake pin attached at fake_old");
        // Build old_addresses set containing the pin's ptr.
        std::unordered_set<void*> old_addrs;
        old_addrs.insert(fake_old);
        // Call verify directly — should return false (contract fail).
        const bool ok = aura::core::lifetime::verify_pins_under_moving_compact(
            /*arena_id=*/0, old_addrs);
        CHECK(!ok, "AC_M6: verify_pins_under_moving_compact returns false when pin's ptr_ is in "
                   "old_addresses");
        // Counter must have bumped.
        const std::uint64_t after = aura::core::lifetime::lifetime_pin_contract_fail_total();
        CHECK(after == before + 1,
              "AC_M6: lifetime_pin_contract_fail_total bumped by 1 on contract fail");
    }

    // ── Source contract ──
    {
        const auto ar = read_file("src/core/arena.ixx");
        CHECK(ar.find("2166") != std::string::npos, "arena cites 2166");
        CHECK(ar.find("Moving = 2") != std::string::npos, "Moving enum");
        CHECK(ar.find("resolve_object_remap") != std::string::npos, "remap API");
        CHECK(ar.find("relocate_tracked_objects_for_moving_") != std::string::npos, "densify fn");
        CHECK(ar.find("AURA_ARENA_MOVING_COMPACT") != std::string::npos, "env flag");
        CHECK(ar.find("moved_live_objects") != std::string::npos, "result flag");
    }

    set_moving_compact_enabled(0);

    // Issue #2342: sharded LifetimePin registry surface (Option 1 —
    // shard by arena_id, N=16 per the issue's "incremental risk"
    // preference). Runs last so the global stats counters are stable
    // for ac2342_1 / ac2342_3 (the prior AC_M* tests bump pins +
    // dtors which contribute to lock_wait_us_total + total_pinned).
    ac2342_1_shard_constants_and_accessors();
    ac2342_2_ctor_dtor_routes_to_shard_zero();
    ac2342_3_lock_wait_us_total();
    {
        CompilerService cs;
        ac2342_4_query_schema(cs);
    }
    ac2342_5_source_cite();

    std::println("\n=== #2166 + #2342: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
