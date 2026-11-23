// Issue #5404: the human approval point. A pending object has a
// deadline; expiry is a denial, an explicit decision is approved/denied,
// and a task with the approval flag cannot be claimed until approved.

#include "bus/approval.hh"
#include "bus/tasks.hh"

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
    ApprovalBoard board;
    std::string why;

    ApprovalRecord rec{};
    check(board.request(1, 42, 100, 1000, &rec, &why), "request a pending approval");
    check(rec.state == ApprovalState::Pending && rec.deadline_ms == 1100,
          "the pending deadline is now + timeout");
    check(board.has_pending(), "the board reports a pending approval");
    std::vector<ApprovalRecord> expired;
    check(board.expire(1099, &expired) == 0, "a live approval does not expire early");
    check(board.expire(1100, &expired) == 1, "the deadline expires the approval");
    check(expired.size() == 1 && expired[0].state == ApprovalState::Timeout,
          "the expired approval is a timeout");
    check(!board.has_pending(), "no pending approval remains");

    ApprovalRecord granted{};
    check(board.decide(2, 1, 7, 500, &granted, &why), "approve an object");
    check(granted.state == ApprovalState::Approved && granted.actor == 7,
          "the approval records the actor");
    ApprovalRecord denied{};
    check(board.decide(3, 2, 7, 500, &denied, &why), "deny an object");
    check(denied.state == ApprovalState::Denied, "the denial is recorded");
    check(!board.decide(4, 9, 7, 500, &denied, &why), "an unknown decision is refused");

    // The task board carries the approval flag through post/claim.
    TaskBoard tasks;
    Task task{};
    TaskRecord event{};
    check(tasks.post(1, 0, 77, kTaskNeedsApproval, &task, &event, &why),
          "post an approval-gated task");
    check(task.flags == kTaskNeedsApproval, "the task keeps its flag");
    Task open{};
    check(tasks.next_open(&open) && open.id == task.id, "the gated task is the next open");
    check(tasks.claim(1, task.id, 1000, 1000, &open, &event, &why),
          "the board itself claims it (the gate is in the coordinator)");

    if (g_failed == 0)
        std::printf("test_bus_approval: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
