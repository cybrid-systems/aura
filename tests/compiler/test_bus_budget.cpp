// Issue #5403: task/global token, wall-clock and failure budgets.
// Exceeding any limit trips the task or the global ledger and refuses
// the next charge; there is no unlimited production default.

#include "bus/budget.hh"

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
    BudgetLedger ledger;
    BudgetPolicy policy{};
    policy.task = {10, 100, 2};
    policy.global = {20, 200, 3};
    ledger.configure(policy);
    std::string why;
    ledger.start(1, 1000);

    BudgetUsage task{};
    BudgetUsage global{};
    check(ledger.charge(1, 5, 0, 0, 1000, &task, &global, &why), "a charge under the limit passes");
    check(task.tokens == 5 && !task.tripped, "the task usage accumulates");
    check(!ledger.charge(1, 6, 0, 0, 1000, &task, &global, &why), "a token overrun trips the task");
    check(task.tripped && task.reason.find("token") != std::string::npos,
          "the task names the token breach");
    check(!ledger.global_tripped(), "the global ledger is still under its limit");

    BudgetLedger failures;
    BudgetPolicy fp{};
    fp.task = {100, 1000, 2};
    fp.global = {1000, 10000, 5};
    failures.configure(fp);
    failures.start(2, 0);
    check(failures.charge(2, 0, 0, 2, 0, &task, &global, &why), "two failures are allowed");
    check(!failures.charge(2, 0, 0, 1, 0, &task, &global, &why),
          "the third failure trips the task");
    check(task.tripped && task.reason.find("failure") != std::string::npos,
          "the task names the failure breach");

    BudgetLedger wall;
    BudgetPolicy wp{};
    wp.task = {100, 50, 10};
    wp.global = {1000, 1000, 10};
    wall.configure(wp);
    wall.start(3, 100);
    check(wall.charge(3, 0, 0, 0, 150, &task, &global, &why), "a live wall clock passes");
    check(!wall.charge(3, 0, 0, 0, 151, &task, &global, &why),
          "the wall-clock overrun trips the task");

    BudgetLedger global_ledger;
    BudgetPolicy gp{};
    gp.task = {1000, 1000, 10};
    gp.global = {20, 1000, 10};
    global_ledger.configure(gp);
    global_ledger.start(4, 0);
    check(!global_ledger.charge(4, 21, 0, 0, 0, &task, &global, &why),
          "a global token overrun trips the global ledger");
    check(global_ledger.global_tripped(), "the global ledger reports tripped");

    if (g_failed == 0)
        std::printf("test_bus_budget: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
