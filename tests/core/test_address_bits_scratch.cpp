// @category: unit
// @reason: Issue #4936 — address-bit helpers, moving pointer slots, and
//          std::array scratch buffers keep the old values.
//
//   AC1: bit_cast round-trips a pointer and an integer address
//   AC2: moving_pointer_slot is the address of the pointer object
//   AC3: densify eval-id table is 64 slots and publishes per id
//   AC4: float, radix, anonymous feedback key, and evo run id spellings

#include "compiler/evo_log_4910.hh"
#include "compiler/float_repr.hh"
#include "compiler/type_feedback.h"
#include "core/address_bits.hh"
#include "core/densify_consistency_report.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
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
    int obj = 7;
    int* ptr = &obj;
    expect(aura::core::address_bits(ptr) ==
               aura::core::address_bits(
                   aura::core::pointer_from_address_bits<int*>(aura::core::address_bits(ptr))),
           "pointer bits round-trip");
    const auto bits = static_cast<std::uintptr_t>(0x1000);
    expect(aura::core::pointer_from_address_bits<void*>(bits) ==
               aura::core::pointer_from_address_bits<void*>(bits),
           "integer address is stable");
    expect(aura::core::address_bits(aura::core::pointer_from_address_bits<const void*>(bits)) ==
               bits,
           "integer address round-trips");

    int other = 9;
    void** slot = aura::core::moving_pointer_slot(ptr);
    expect(slot == reinterpret_cast<void**>(&ptr), "slot is the pointer object");
    *slot = &other;
    expect(ptr == &other, "write through the slot updates the pointer");

    using aura::core::densify_consistency::arm_densify_in_flight;
    using aura::core::densify_consistency::clear_densify_in_flight;
    using aura::core::densify_consistency::densify_in_flight_for;
    using aura::core::densify_consistency::g_densify_eval_slots;
    using aura::core::densify_consistency::kDensifyEvalSlotCount;
    using aura::core::densify_consistency::last_densify_dual_epoch_ok_for;
    using aura::core::densify_consistency::last_densify_envframe_ok_for;
    using aura::core::densify_consistency::note_last_densify_result_for;
    using aura::core::densify_consistency::reset_densify_eval_slots_for_test;
    expect(g_densify_eval_slots.size() == kDensifyEvalSlotCount && kDensifyEvalSlotCount == 64,
           "densify id table has 64 slots");
    reset_densify_eval_slots_for_test();
    const int id_a = 1;
    const int id_b = 2;
    note_last_densify_result_for(&id_a, false, true);
    note_last_densify_result_for(&id_b, true, false);
    expect(!last_densify_envframe_ok_for(&id_a) && last_densify_dual_epoch_ok_for(&id_a),
           "id A envframe reject is stored");
    expect(last_densify_envframe_ok_for(&id_b) && !last_densify_dual_epoch_ok_for(&id_b),
           "id B dual-epoch reject is stored");
    expect(last_densify_envframe_ok_for(&obj), "unknown id is not a reject");
    arm_densify_in_flight(&id_a);
    expect(densify_in_flight_for(&id_a) && !densify_in_flight_for(&id_b), "in-flight is per id");
    clear_densify_in_flight(&id_a);
    expect(!densify_in_flight_for(&id_a), "clear drops in-flight");
    reset_densify_eval_slots_for_test();

    expect(aura::compiler::format_float_shortest(1.0) == "1.0", "1.0 keeps a decimal");
    expect(aura::compiler::format_float_shortest(1.5) == "1.5", "1.5");
    expect(aura::compiler::format_float_shortest(0.0) == "0.0", "0.0");
    expect(aura::compiler::format_float_shortest(std::numeric_limits<double>::quiet_NaN()) == "nan",
           "nan is canonical");
    expect(aura::compiler::format_int_radix(255, 16) == "ff", "255 radix 16");
    expect(aura::compiler::format_int_radix(-255, 16) == "-ff", "negative radix");
    expect(aura::compiler::format_int_radix(10, 10) == "10", "radix 10");
    expect(aura::compiler::format_int_radix(std::numeric_limits<std::int64_t>::min(), 16)
               .starts_with('-'),
           "INT64_MIN radix 16");

    aura::compiler::feedback::EnterInfo info;
    info.key = &obj;
    const auto key = aura::compiler::feedback::detail::map_key(info);
    std::array<char, 32> expect_key{};
    std::snprintf(expect_key.data(), expect_key.size(), "#%lx",
                  static_cast<unsigned long>(aura::core::address_bits(info.key)));
    expect(key == expect_key.data(), "anonymous feedback key");

    ::unsetenv("AURA_EVO_RUN");
    const auto run = aura::evo::default_run_id();
    expect(run.size() >= 18 && run.find('T') != std::string::npos &&
               run.find('Z') != std::string::npos,
           "evo run id uses the scratch buffer");

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
