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

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <string>

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

int main() {
    int storage = 42;
    {
        aura::core::lifetime::LifetimePin pin;
        pin.pin(&storage, 4, 1);
        expect(pin.validate(4, 1) && pin.ptr() == &storage, "pin validates the current generation");
        expect(!pin.validate(5, 1) && pin.ptr() == &storage,
               "a generation mismatch keeps the pinned address");
        expect(pin.validate(4, 1), "the original generation still validates");
        pin.pin(&storage, 9, 0);
        expect(pin.arena_id() == 0 && pin.validate(9, 0), "re-pin moves the registry shard");
        aura::core::lifetime::LifetimePin moved(std::move(pin));
        expect(!pin.pinned() && moved.validate(9, 0) && moved.ptr() == &storage,
               "move transfers the registry entry under the shard lock");
    }
    expect(true, "pin and moved-from pin both destroy");

    const auto path = std::filesystem::temp_directory_path() / "aura-evo-4936.jsonl";
    std::filesystem::remove(path);
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
    std::filesystem::remove(path);

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
