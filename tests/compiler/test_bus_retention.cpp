// Issue #5460: the task and approval boards keep a bounded terminal
// window, index their hot lookups, and compact their checkpoints.

#include "bus/approval.hh"
#include "bus/tasks.hh"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

namespace {
int g_failed = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

std::string temp_path() {
    char tmpl[] = "/tmp/aura-retention-XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0)
        ::close(fd);
    return std::string(tmpl);
}

using namespace aura::bus;
} // namespace

int main() {
    std::string why;

    // ── The task board keeps only the newest terminal window ──
    TaskBoard board;
    board.set_terminal_retention(8);
    for (int i = 0; i < 40; ++i) {
        Task task{};
        TaskRecord event{};
        check(board.post(1, 0, static_cast<std::uint64_t>(i), 0, &task, &event, &why),
              "post a task");
        check(board.claim(1, task.id, 1000, 1000, &task, &event, &why), "claim the task");
        check(board.complete(1, task.id, 7, 1000, &task, &event, &why), "complete the task");
    }
    check(board.size() == 8, "only the terminal window stays on the board");
    check(board.terminal_count() == 8, "the terminal window is the configured size");
    check(!board.get(1, nullptr), "an evicted terminal task is gone");
    check(board.get(40, nullptr), "the newest terminal task stays");
    check(!board.has_leases(), "finished tasks leave the lease index");
    check(board.open_count() == 0, "no open tasks remain");

    Task task{};
    TaskRecord event{};
    check(board.post(1, 0, 99, 0, &task, &event, &why), "post after the window moved");
    check(task.id == 41, "evicted ids are never reused");
    check(board.open_count() == 1, "the open index sees the new task");
    Task next{};
    check(board.next_open(&next) && next.id == 41, "next_open uses the open index");

    // ── The lease index only expires due leases ──
    Task first{};
    Task second{};
    TaskRecord ignored{};
    check(board.post(1, 0, 1, 0, &first, &ignored, &why), "post lease task one");
    check(board.post(1, 0, 2, 0, &second, &ignored, &why), "post lease task two");
    check(board.claim(1, first.id, 100, 1000, &first, &ignored, &why), "claim lease task one");
    check(board.claim(1, second.id, 500, 1000, &second, &ignored, &why), "claim lease task two");
    std::vector<TaskRecord> expired;
    check(board.expire(1200, &expired) == 1, "only the due lease expires");
    check(board.get(first.id, &first) && first.state == TaskState::Open,
          "the due lease returns to open");
    check(board.get(second.id, &second) && second.state == TaskState::Claimed,
          "the later lease stays claimed");

    // ── A compacted checkpoint resumes the window and the next id ──
    const std::string path = temp_path();
    {
        TaskBoard disk;
        check(disk.resume(path, &why), "create the task checkpoint");
        disk.set_terminal_retention(8);
        for (int i = 0; i < 100; ++i) {
            Task t{};
            TaskRecord e{};
            check(disk.post(1, 0, static_cast<std::uint64_t>(i), 0, &t, &e, &why), "disk post");
            check(disk.claim(1, t.id, 1000, 1000, &t, &e, &why), "disk claim");
            check(disk.complete(1, t.id, 7, 1000, &t, &e, &why), "disk complete");
        }
        check(disk.take_compacted(), "the task checkpoint was compacted");
        Task open{};
        Task claimed{};
        TaskRecord e{};
        check(disk.post(1, 0, 100, 0, &open, &e, &why), "leave one open task");
        check(disk.post(1, 0, 101, 0, &claimed, &e, &why), "leave one claimed task");
        check(disk.claim(1, claimed.id, 10000, 2000, &claimed, &e, &why),
              "claim the surviving task");
    }
    {
        TaskBoard disk;
        disk.set_terminal_retention(8);
        check(disk.resume(path, &why), "resume the compacted checkpoint");
        check(disk.size() == 10, "the window plus live tasks resumes");
        check(!disk.get(1, nullptr), "compacted history stays gone");
        check(disk.get(100, nullptr), "a terminal task inside the window resumes");
        Task claimed{};
        check(disk.get(102, &claimed) && claimed.state == TaskState::Claimed,
              "a claimed task resumes claimed");
        check(claimed.deadline_ms == 12000, "the lease deadline resumes");
        Task open{};
        check(disk.next_open(&open) && open.id == 101, "the open task resumes");
        Task fresh{};
        TaskRecord e{};
        check(disk.post(1, 0, 102, 0, &fresh, &e, &why), "post after resume");
        check(fresh.id == 103, "the next id survives compaction");
    }
    (void)::unlink(path.c_str());

    // ── Approvals: pending index and bounded decided window ──
    ApprovalBoard approvals;
    approvals.set_retention(4);
    for (std::uint64_t i = 1; i <= 20; ++i) {
        ApprovalRecord rec{};
        check(approvals.decide(i, 1, 7, 500, &rec, &why), "decide an approval");
    }
    check(approvals.decided_count() >= 4 && approvals.decided_count() <= 8,
          "the decided window stays bounded");
    check(!approvals.get(1, nullptr), "an evicted decision is gone");
    check(approvals.get(20, nullptr), "the newest decision stays");

    ApprovalBoard pending;
    ApprovalRecord rec{};
    check(pending.request(1, 7, 100, 1000, &rec, &why), "request approval one");
    check(pending.request(2, 7, 500, 1000, &rec, &why), "request approval two");
    std::vector<ApprovalRecord> timed_out;
    check(pending.expire(1200, &timed_out) == 1, "only the due approval times out");
    check(pending.get(1, &rec) && rec.state == ApprovalState::Timeout,
          "the due approval timed out");
    check(pending.get(2, &rec) && rec.state == ApprovalState::Pending,
          "the later approval stays pending");
    check(pending.has_pending(), "a pending approval is indexed");
    check(pending.expire(1500, &timed_out) == 1, "the later approval times out");
    check(!pending.has_pending(), "the pending index drains");

    ApprovalBoard forgetful;
    check(forgetful.decide(9, 2, 7, 500, &rec, &why), "decide a forgettable object");
    check(forgetful.decided_count() == 1, "the decision is counted");
    forgetful.forget(9);
    check(!forgetful.get(9, nullptr), "forget drops the record");
    check(forgetful.decided_count() == 0, "forget updates the decided count");

    if (g_failed == 0)
        std::printf("test_bus_retention: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
