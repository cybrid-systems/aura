// ──────────────────────────────────────────────────────────────
//  contract.hh — executable acceptance contracts (#5402)
//
//  A contract is attached to one task: a test/suite reference and a
//  metric floor. The task cannot start until the contract is approved,
//  and it cannot complete until the test passed and the metric reached
//  the floor. The fingerprint binds the contract fields.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_CONTRACT_HH
#define AURA_BUS_CONTRACT_HH

#include <cstdint>
#include <map>
#include <string>

namespace aura::bus {

struct Contract {
    std::uint64_t task = 0;
    std::uint64_t test_ref = 0;     // an opaque test/suite reference
    std::uint64_t metric_floor = 0; // the required metric (ppm/units)
    std::uint64_t fingerprint = 0;  // binds task/test/floor
    std::uint64_t tests_passed = 0;
    std::uint64_t metric = 0;
    bool has_evidence = false;
};

class ContractBoard {
public:
    bool create(std::uint64_t task, std::uint64_t test_ref, std::uint64_t metric_floor,
                Contract* out, std::string* why);
    bool get(std::uint64_t task, Contract* out) const;
    // Record the terminal evidence for a task's contract.
    bool evidence(std::uint64_t task, std::uint64_t tests_passed, std::uint64_t metric,
                  Contract* out, std::string* why);
    // True when the contract is fully satisfied: a test result says pass
    // and the metric is at or above the floor.
    bool satisfied(std::uint64_t task, std::string* why) const;
    // #5427: re-apply a persisted contract (checkpoint replay).
    void restore(const Contract& contract) { contracts_[contract.task] = contract; }
    [[nodiscard]] const std::map<std::uint64_t, Contract>& all() const { return contracts_; }

private:
    std::map<std::uint64_t, Contract> contracts_;
};

} // namespace aura::bus

#endif
