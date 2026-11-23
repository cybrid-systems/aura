// ──────────────────────────────────────────────────────────────
//  tasks.hh — the bus task board and its leases (#5399)
//
//  The board is a replay of append-only TaskRecord events in the
//  tasks channel. Every mutation is checked by the coordinator
//  thread, written to the image (best effort) and to a checkpoint
//  next to the socket so a restarted host resumes the board.
// ──────────────────────────────────────────────────────────────
#ifndef AURA_BUS_TASKS_HH
#define AURA_BUS_TASKS_HH

#include "bus/image_format.hh"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace aura::bus {

// #5404: a task with this flag cannot be claimed until its object is
// approved through bus:approve.
inline constexpr std::uint64_t kTaskNeedsApproval = 1ull << 0;

struct Task {
    std::uint64_t id = 0;
    std::uint64_t parent = 0;
    TaskState state = TaskState::Open;
    std::uint64_t owner = 0; // the holder's root grant token
    std::uint64_t lease_epoch = 0;
    std::uint64_t deadline_ms = 0;
    std::uint64_t ref = 0;   // contract fingerprint / worldline / result
    std::uint64_t flags = 0; // #5404: kTaskNeedsApproval
};

// The task board. Only the coordinator thread calls the mutators, so
// no lock is needed; a peer never touches it directly.
class TaskBoard {
public:
    TaskBoard() = default;
    TaskBoard(const TaskBoard&) = delete;
    TaskBoard& operator=(const TaskBoard&) = delete;
    ~TaskBoard();
    // Load the checkpoint at path (or start empty when it does not
    // exist). A malformed/foreign file fails closed.
    bool resume(const std::string& path, std::string* why);
    [[nodiscard]] const std::string& checkpoint_path() const { return checkpoint_path_; }

    // Publish a task. parent/ref are opaque to the board.
    bool post(std::uint64_t actor, std::uint64_t parent, std::uint64_t ref, std::uint64_t flags,
              Task* out, TaskRecord* event, std::string* why);
    // Claim a specific task (task_id != 0) or the oldest open task
    // (task_id == 0). A claimed task cannot be claimed again until its
    // lease expires or it is released.
    bool claim(std::uint64_t actor, std::uint64_t task_id, std::uint64_t lease_ms,
               std::uint64_t now_ms, Task* out, TaskRecord* event, std::string* why);
    // Renew the holder's lease. The holder must match and the lease
    // must not already have expired.
    bool heartbeat(std::uint64_t actor, std::uint64_t task_id, std::uint64_t lease_ms,
                   std::uint64_t now_ms, Task* out, TaskRecord* event, std::string* why);
    // Finish the task. A completed/failed task is terminal.
    bool complete(std::uint64_t actor, std::uint64_t task_id, std::uint64_t ref,
                  std::uint64_t now_ms, Task* out, TaskRecord* event, std::string* why);
    bool fail(std::uint64_t actor, std::uint64_t task_id, std::uint64_t ref, std::uint64_t now_ms,
              Task* out, TaskRecord* event, std::string* why);
    // Return a claimed task to the open state (clean detach / explicit
    // release). The holder must match.
    bool release(std::uint64_t actor, std::uint64_t task_id, Task* out, TaskRecord* event,
                 std::string* why);
    // #5402: update the flags of an open task (e.g. contract approval).
    bool set_flags(std::uint64_t task_id, std::uint64_t flags, Task* out, TaskRecord* event,
                   std::string* why);

    // Requeue every claim whose deadline passed. Appends one Expire
    // event per task to *events.
    std::size_t expire(std::uint64_t now_ms, std::vector<TaskRecord>* events);

    [[nodiscard]] bool get(std::uint64_t id, Task* out) const;
    // The oldest open task (the claim(0) candidate), without claiming.
    [[nodiscard]] bool next_open(Task* out) const;
    [[nodiscard]] std::vector<Task> list() const;
    [[nodiscard]] bool has_leases() const;
    [[nodiscard]] std::size_t size() const { return tasks_.size(); }

    // Replay one event into the board (used by resume/tests).
    void apply(const TaskRecord& event);

private:
    bool persist(const TaskRecord& event);

    std::map<std::uint64_t, Task> tasks_;
    std::uint64_t next_id_ = 1;
    std::string checkpoint_path_;
    std::FILE* checkpoint_ = nullptr;
};

} // namespace aura::bus

#endif
