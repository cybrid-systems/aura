// ──────────────────────────────────────────────────────────────
//  contract.cpp — executable acceptance contracts (#5402)
// ──────────────────────────────────────────────────────────────
#include "bus/contract.hh"

namespace aura::bus {
namespace {

    std::uint64_t fnv_u64(std::uint64_t hash, std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= (value >> (i * 8)) & 0xffull;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    std::uint64_t fingerprint(std::uint64_t task, std::uint64_t test_ref,
                              std::uint64_t metric_floor) {
        std::uint64_t hash = 14695981039346656037ull;
        hash = fnv_u64(hash, task);
        hash = fnv_u64(hash, test_ref);
        hash = fnv_u64(hash, metric_floor);
        return hash;
    }

} // namespace

bool ContractBoard::create(std::uint64_t task, std::uint64_t test_ref, std::uint64_t metric_floor,
                           Contract* out, std::string* why) {
    if (task == 0) {
        if (why)
            *why = "contract task is zero";
        return false;
    }
    if (contracts_.find(task) != contracts_.end()) {
        if (why)
            *why = "contract already exists";
        return false;
    }
    Contract contract{};
    contract.task = task;
    contract.test_ref = test_ref;
    contract.metric_floor = metric_floor;
    contract.fingerprint = fingerprint(task, test_ref, metric_floor);
    contracts_[task] = contract;
    if (out)
        *out = contract;
    return true;
}

bool ContractBoard::get(std::uint64_t task, Contract* out) const {
    const auto it = contracts_.find(task);
    if (it == contracts_.end())
        return false;
    if (out)
        *out = it->second;
    return true;
}

bool ContractBoard::evidence(std::uint64_t task, std::uint64_t tests_passed, std::uint64_t metric,
                             Contract* out, std::string* why) {
    const auto it = contracts_.find(task);
    if (it == contracts_.end()) {
        if (why)
            *why = "contract is missing";
        return false;
    }
    it->second.tests_passed = tests_passed;
    it->second.metric = metric;
    it->second.has_evidence = true;
    if (out)
        *out = it->second;
    return true;
}

bool ContractBoard::satisfied(std::uint64_t task, std::string* why) const {
    const auto it = contracts_.find(task);
    if (it == contracts_.end()) {
        if (why)
            *why = "contract is missing";
        return false;
    }
    const Contract& contract = it->second;
    if (!contract.has_evidence) {
        if (why)
            *why = "contract has no evidence";
        return false;
    }
    if (contract.tests_passed == 0) {
        if (why)
            *why = "contract tests did not pass";
        return false;
    }
    if (contract.metric < contract.metric_floor) {
        if (why)
            *why = "contract metric " + std::to_string(contract.metric) + " is below the floor " +
                   std::to_string(contract.metric_floor);
        return false;
    }
    return true;
}

} // namespace aura::bus
