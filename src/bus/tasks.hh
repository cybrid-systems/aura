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

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
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
    // #5460: terminal tasks are kept as a bounded window so a long
    // board does not make every poll wakeup scan the whole history.
    static constexpr std::size_t kDefaultTerminalRetention = 4096;
    static constexpr std::size_t kMinTerminalRetention = 1;

    TaskBoard() = default;
    TaskBoard(const TaskBoard&) = delete;
    TaskBoard& operator=(const TaskBoard&) = delete;
    ~TaskBoard();
    // Load the checkpoint at path (or start empty when it does not
    // exist). A malformed/foreign file fails closed.
    bool resume(const std::string& path, std::string* why);
    [[nodiscard]] const std::string& checkpoint_path() const { return checkpoint_path_; }

    // #5460: tests use a small window; production keeps the default.
    void set_terminal_retention(std::size_t count);

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
    // #5439: every open task in id order. A no-id claim walks this list
    // and skips the candidates its gates refuse instead of stopping at
    // the first blocked one.
    [[nodiscard]] std::vector<Task> open_tasks() const;
    // #5422: the open tasks, for a supervisor that only starts workers
    // when there is work. Claimed tasks are not counted.
    [[nodiscard]] std::size_t open_count() const { return open_ids_.size(); }
    [[nodiscard]] std::vector<Task> claimed_tasks() const;
    [[nodiscard]] std::vector<Task> list() const;
    [[nodiscard]] bool has_leases() const { return !claimed_ids_.empty(); }
    [[nodiscard]] std::size_t size() const { return tasks_.size(); }
    [[nodiscard]] std::size_t terminal_count() const { return terminal_order_.size(); }

    // #5460: ids dropped by the terminal-retention window since the
    // last call, so the coordinator can drop their gate state too.
    std::size_t take_pruned(std::vector<std::uint64_t>* out);
    // True once after the task checkpoint was rewritten compactly.
    bool take_compacted();

    // Replay one event into the board (used by resume/tests).
    void apply(const TaskRecord& event);

private:
    bool persist(const TaskRecord& event);
    void index_task(const Task& task);
    void unindex_task(const Task& task);
    void maybe_prune_terminal();
    bool compact_checkpoint();

    std::map<std::uint64_t, Task> tasks_;
    // #5460: the hot lookups are indexes, not full-board scans.
    std::set<std::uint64_t> open_ids_;
    std::set<std::uint64_t> claimed_ids_;
    std::set<std::pair<std::uint64_t, std::uint64_t>> leases_; // (deadline, id)
    std::deque<std::uint64_t> terminal_order_;
    std::vector<std::uint64_t> pruned_ids_;
    std::size_t terminal_retention_ = kDefaultTerminalRetention;
    std::size_t pruned_since_compact_ = 0;
    std::size_t stream_records_ = 0;
    bool compacted_ = false;
    bool checkpoint_required_ = false;
    std::uint64_t next_id_ = 1;
    std::string checkpoint_path_;
    std::FILE* checkpoint_ = nullptr;
};

} // namespace aura::bus

#endif
