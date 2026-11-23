// Issue #5402: executable acceptance contracts. A contract binds a
// task to a test reference and a metric floor; completion requires a
// passing test result and a metric at or above the floor.

#include "bus/contract.hh"

#include <cstdio>
#include <string>

namespace {
int g_failed = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}
using namespace aura::bus;
} // namespace

int main() {
    ContractBoard board;
    Contract contract{};
    std::string why;

    check(board.create(5, 42, 1000, &contract, &why), "create a contract");
    check(contract.task == 5 && contract.test_ref == 42 && contract.metric_floor == 1000,
          "the contract fields are kept");
    check(contract.fingerprint != 0, "the contract has a fingerprint");
    check(!board.create(5, 43, 1000, &contract, &why), "a duplicate contract is refused");
    check(!board.get(6, &contract), "an unknown task has no contract");

    check(!board.satisfied(5, &why), "a contract without evidence is not satisfied");
    check(board.evidence(5, 1, 999, &contract, &why), "record failing-metric evidence");
    check(!board.satisfied(5, &why), "a metric below the floor is refused");
    check(board.evidence(5, 0, 1000, &contract, &why), "record failing-test evidence");
    check(!board.satisfied(5, &why), "a failing test is refused");
    check(board.evidence(5, 1, 1000, &contract, &why), "record passing evidence at the floor");
    check(board.satisfied(5, &why), "passing tests and the floor satisfy the contract");
    check(board.get(5, &contract) && contract.has_evidence && contract.tests_passed == 1 &&
              contract.metric == 1000,
          "the evidence is readable");

    if (g_failed == 0)
        std::printf("test_bus_contract: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
