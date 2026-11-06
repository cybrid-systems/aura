// @category: unit
// @reason: Issue #4936 — pin-registry locks and the evolution log format run
//          from the current headers, with no snapshot library.
//
//   AC1: pin, generation check, shard move, and move-construction take the lock
//   AC2: a mismatched generation does not drop the pinned address
//   AC3: the log escapes a control character and formats dur_ms without a C buffer
//   AC4: KEEP advances the tip; a later line keeps that parent

#include "compiler/evo_log_4910.hh"
#include "core/lifetime_pin.hh"
#include "core/resource_quota.hh"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <unistd.h>

namespace {

int g_failed = 0;

void expect(bool ok, const char* label) {
    if (!ok) {
        std::println(std::cerr, "FAIL {}", label);
        ++g_failed;
    } else {
        std::println("PASS {}", label);
    }
}

} // namespace

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("aura-evo-4936-" + std::to_string(::getpid()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

int main() {
    static_assert(aura::core::resource_quota::ResourceQuota::dimension_name_at(0) == "memory");
    static_assert(aura::core::resource_quota::ResourceQuota::dimension_name_at(1) == "fibers");
    static_assert(aura::core::resource_quota::ResourceQuota::dimension_name_at(4) ==
                  "offload_inflight");
    static_assert(aura::core::resource_quota::ResourceQuota::dimension_name_at(5) == "unknown");
    static_assert(aura::core::resource_quota::ResourceQuota::dimension_name_at(-1) == "unknown");

    int storage = 42;
    const auto pins_before = aura::core::lifetime::live_pin_count();
    {
        aura::core::lifetime::LifetimePin pin;
        expect(aura::core::lifetime::live_pin_count() == pins_before,
               "an unpinned object is not a live registry pin");
        pin.pin(&storage, 4, 1);
        expect(aura::core::lifetime::live_pin_count() == pins_before + 1,
               "pin increases the registry count");
        expect(pin.validate(4, 1) && pin.ptr() == &storage, "pin validates the current generation");
        expect(!pin.validate(5, 1) && pin.ptr() == &storage,
               "a generation mismatch keeps the pinned address");
        expect(pin.validate(4, 1), "the original generation still validates");
        pin.pin(&storage, 9, 0);
        expect(pin.arena_id() == 0 && pin.validate(9, 0) &&
                   aura::core::lifetime::live_pin_count() == pins_before + 1,
               "re-pin moves the registry shard without adding a pin");
        aura::core::lifetime::LifetimePin moved(std::move(pin));
        expect(!pin.pinned() && moved.validate(9, 0) && moved.ptr() == &storage &&
                   aura::core::lifetime::live_pin_count() == pins_before + 1,
               "move transfers the registry entry under the shard lock");
    }
    expect(aura::core::lifetime::live_pin_count() == pins_before,
           "destroying the pin removes it from the registry");

    const TempDir dir;
    const auto path = dir.path / "log.jsonl";
    ::setenv("AURA_EVO_LOG", path.c_str(), 1);
    aura::evo::log_reset_for_test();
    aura::evo::Entry first;
    first.kind = "mutate:rebind";
    first.target = "f";
    first.decision = "KEEP";
    first.reason = "ok";
    first.evidence_json = "1";
    const std::string old_src = std::string("a") + '\x01';
    first.old_src = old_src;
    first.new_src = "b";
    first.dur_ms = 1.5;
    const auto id = aura::evo::log_append(first);
    aura::evo::Entry second;
    second.kind = "mutate:rebind";
    second.target = "g";
    second.decision = "ROLLBACK";
    second.reason = "no";
    second.dur_ms = 0.25;
    const auto id2 = aura::evo::log_append(second);
    std::ifstream in(path);
    std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    expect(!id.empty() && file.find(id) != std::string::npos, "KEEP writes a record id");
    expect(file.find("\\u0001") != std::string::npos, "a control character is escaped");
    expect(file.find("\"dur_ms\":1.500") != std::string::npos, "duration is formatted to 3 places");
    expect(!id2.empty() && file.find("\"parent\":\"" + id + "\"") != std::string::npos,
           "the next line's parent is the KEEP tip");
    aura::evo::log_reset_for_test();
    ::unsetenv("AURA_EVO_LOG");

    using aura::core::resource_quota::Dimension;
    using aura::core::resource_quota::ResourceQuota;
    aura::core::resource_quota::set_quota_per_tenant_enabled_for_test(true);
    {
        ResourceQuota quota;
        quota.set_tenant_limit(7, Dimension::Fibers, 1);
        auto held = quota.try_reserve_fiber(7);
        expect(held.has_value() && quota.tenant_used(7, Dimension::Fibers) == 1,
               "the first fiber reserve takes the tenant lock and consumes one slot");
        auto denied = quota.try_reserve_fiber_detailed(7);
        expect(!denied.first.has_value() && denied.second.has_value() &&
                   denied.second->message.find("fibers") != std::string::npos,
               "the second reserve is denied with the constexpr dimension name");
        held.reset();
        expect(quota.tenant_used(7, Dimension::Fibers) == 0,
               "releasing the token returns the tenant slot");
    }
    aura::core::resource_quota::clear_quota_per_tenant_test_override();

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
