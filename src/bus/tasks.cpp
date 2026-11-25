// ──────────────────────────────────────────────────────────────
//  tasks.cpp — the bus task board and its leases (#5399)
// ──────────────────────────────────────────────────────────────
#include "bus/tasks.hh"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <unistd.h>

namespace aura::bus {
namespace {

    constexpr char kTaskCheckpointMagic[8] = {'A', 'U', 'R', 'A', 'T', 'S', 'K', '\n'};
    // v2 adds the next task id, so compacting away old terminal tasks
    // cannot reuse their ids after a restart (#5460).
    constexpr std::uint32_t kTaskCheckpointVersion = 2;
    constexpr std::uint32_t kTaskCheckpointVersionV1 = 1;
    constexpr std::uint64_t kTaskDefaultLeaseMs = 30000;

    struct TaskCheckpointHeaderV1 {
        char magic[8];
        std::uint32_t version;
        std::uint32_t reserved;
        std::uint64_t count;
    };
    static_assert(sizeof(TaskCheckpointHeaderV1) == 24);

    struct TaskCheckpointHeaderV2 {
        char magic[8];
        std::uint32_t version;
        std::uint32_t reserved;
        std::uint64_t count;
        std::uint64_t next_id;
    };
    static_assert(sizeof(TaskCheckpointHeaderV2) == 32);

    bool is_terminal(TaskState state) {
        return state == TaskState::Done || state == TaskState::Failed;
    }

    const char* task_state_reason(TaskState state) {
        switch (state) {
            case TaskState::Open:
                return "task-open";
            case TaskState::Claimed:
                return "task-claimed";
            case TaskState::Done:
                return "task-done";
            case TaskState::Failed:
                return "task-failed";
        }
        return "task-unknown";
    }

    TaskRecord state_record(const Task& task) {
        TaskRecord record{};
        record.task_id = task.id;
        record.parent = task.parent;
        record.actor = task.owner;
        record.lease_epoch = task.lease_epoch;
        record.deadline_ms = task.deadline_ms;
        record.ref = task.ref;
        record.flags = task.flags;
        switch (task.state) {
            case TaskState::Claimed:
                record.kind = static_cast<std::uint64_t>(TaskEventKind::Claim);
                break;
            case TaskState::Done:
                record.kind = static_cast<std::uint64_t>(TaskEventKind::Complete);
                break;
            case TaskState::Failed:
                record.kind = static_cast<std::uint64_t>(TaskEventKind::Fail);
                break;
            default:
                break;
        }
        return record;
    }

} // namespace

TaskBoard::~TaskBoard() {
    if (checkpoint_ != nullptr)
        std::fclose(checkpoint_);
}

void TaskBoard::set_terminal_retention(std::size_t count) {
    terminal_retention_ = std::max<std::size_t>(count, kMinTerminalRetention);
}

void TaskBoard::index_task(const Task& task) {
    switch (task.state) {
        case TaskState::Open:
            open_ids_.insert(task.id);
            break;
        case TaskState::Claimed:
            claimed_ids_.insert(task.id);
            if (task.deadline_ms != 0)
                leases_.insert({task.deadline_ms, task.id});
            break;
        case TaskState::Done:
        case TaskState::Failed:
            break;
    }
}

void TaskBoard::unindex_task(const Task& task) {
    switch (task.state) {
        case TaskState::Open:
            open_ids_.erase(task.id);
            break;
        case TaskState::Claimed:
            claimed_ids_.erase(task.id);
            if (task.deadline_ms != 0)
                leases_.erase({task.deadline_ms, task.id});
            break;
        case TaskState::Done:
        case TaskState::Failed:
            break;
    }
}

bool TaskBoard::resume(const std::string& path, std::string* why) {
    checkpoint_path_ = path;
    checkpoint_required_ = true;
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    if (f == nullptr) {
        if (errno != ENOENT) {
            if (why)
                *why = std::string("task checkpoint open failed: ") + std::strerror(errno);
            return false;
        }
        f = std::fopen(path.c_str(), "w+b");
        if (f == nullptr) {
            if (why)
                *why = std::string("task checkpoint create failed: ") + std::strerror(errno);
            return false;
        }
        TaskCheckpointHeaderV2 header{};
        std::memcpy(header.magic, kTaskCheckpointMagic, sizeof(header.magic));
        header.version = kTaskCheckpointVersion;
        header.count = 0;
        header.next_id = next_id_;
        if (std::fwrite(&header, sizeof(header), 1, f) != 1) {
            if (why)
                *why = "task checkpoint header write failed";
            std::fclose(f);
            return false;
        }
        std::fflush(f);
        checkpoint_ = f;
        return true;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::rewind(f);
    std::uint64_t header_size = 0;
    std::uint64_t header_next_id = 0;
    if (size == 0) {
        // A crash between create and header write must not wedge every
        // later host: an empty file is a fresh board.
        TaskCheckpointHeaderV2 header{};
        std::memcpy(header.magic, kTaskCheckpointMagic, sizeof(header.magic));
        header.version = kTaskCheckpointVersion;
        header.next_id = next_id_;
        if (std::fwrite(&header, sizeof(header), 1, f) != 1) {
            if (why)
                *why = "task checkpoint header write failed";
            std::fclose(f);
            return false;
        }
        std::fflush(f);
        header_size = sizeof(header);
    } else {
        TaskCheckpointHeaderV1 first{};
        if (size < static_cast<long>(sizeof(first)) ||
            std::fread(&first, sizeof(first), 1, f) != 1 ||
            std::memcmp(first.magic, kTaskCheckpointMagic, sizeof(first.magic)) != 0) {
            if (why)
                *why = "task checkpoint is malformed or from another version";
            std::fclose(f);
            return false;
        }
        if (first.version == kTaskCheckpointVersion) {
            TaskCheckpointHeaderV2 header{};
            std::memcpy(header.magic, first.magic, sizeof(header.magic));
            header.version = first.version;
            header.reserved = first.reserved;
            header.count = first.count;
            if (std::fread(&header.next_id, sizeof(header.next_id), 1, f) != 1) {
                if (why)
                    *why = "task checkpoint header is truncated";
                std::fclose(f);
                return false;
            }
            header_next_id = header.next_id;
            header_size = sizeof(header);
        } else if (first.version == kTaskCheckpointVersionV1) {
            header_size = sizeof(first);
        } else {
            if (why)
                *why = "task checkpoint is malformed or from another version";
            std::fclose(f);
            return false;
        }
    }
    // The record stream, not the informational count, is authoritative.
    // A torn tail (a crash mid-append) is dropped so the next append
    // starts a whole record instead of gluing two partial events.
    long valid_end = static_cast<long>(header_size);
    std::size_t records = 0;
    while (true) {
        TaskRecord record{};
        const std::size_t n = std::fread(&record, sizeof(record), 1, f);
        if (n != 1)
            break;
        apply(record);
        ++records;
        valid_end = std::ftell(f);
    }
    if (valid_end < 0 || ::ftruncate(::fileno(f), valid_end) != 0) {
        if (why)
            *why = "task checkpoint truncate failed";
        std::fclose(f);
        return false;
    }
    next_id_ = std::max(next_id_, header_next_id);
    std::fseek(f, 0, SEEK_END);
    checkpoint_ = f;
    stream_records_ = records;
    // The replay appended every event since the last compaction; apply
    // the window now, then rewrite once if the stream carries history
    // beyond the live board.
    maybe_prune_terminal();
    if (stream_records_ > tasks_.size() * 2 + 64)
        (void)compact_checkpoint();
    return true;
}

void TaskBoard::apply(const TaskRecord& event) {
    if (event.task_id == 0)
        return;
    switch (static_cast<TaskEventKind>(event.kind)) {
        case TaskEventKind::Post: {
            Task task{};
            task.id = event.task_id;
            task.parent = event.parent;
            task.ref = event.ref;
            task.flags = event.flags;
            task.state = TaskState::Open;
            const auto found = tasks_.find(task.id);
            if (found != tasks_.end())
                unindex_task(found->second);
            tasks_[task.id] = task;
            index_task(task);
            next_id_ = std::max(next_id_, task.id + 1);
            break;
        }
        case TaskEventKind::Claim: {
            const auto it = tasks_.find(event.task_id);
            if (it == tasks_.end())
                break;
            unindex_task(it->second);
            it->second.state = TaskState::Claimed;
            it->second.owner = event.actor;
            it->second.lease_epoch = event.lease_epoch;
            it->second.deadline_ms = event.deadline_ms;
            index_task(it->second);
            break;
        }
        case TaskEventKind::Heartbeat: {
            const auto it = tasks_.find(event.task_id);
            if (it == tasks_.end() || it->second.state != TaskState::Claimed)
                break;
            if (event.lease_epoch != 0 && event.lease_epoch != it->second.lease_epoch)
                break;
            unindex_task(it->second);
            it->second.deadline_ms = event.deadline_ms;
            index_task(it->second);
            break;
        }
        case TaskEventKind::Complete:
        case TaskEventKind::Fail: {
            const auto it = tasks_.find(event.task_id);
            if (it == tasks_.end())
                break;
            const bool was_terminal = is_terminal(it->second.state);
            unindex_task(it->second);
            it->second.state = static_cast<TaskEventKind>(event.kind) == TaskEventKind::Complete
                                   ? TaskState::Done
                                   : TaskState::Failed;
            it->second.owner = event.actor;
            it->second.ref = event.ref;
            it->second.deadline_ms = 0;
            index_task(it->second);
            if (!was_terminal)
                terminal_order_.push_back(it->second.id);
            break;
        }
        case TaskEventKind::Release:
        case TaskEventKind::Expire:
        case TaskEventKind::Requeue: {
            const auto it = tasks_.find(event.task_id);
            if (it == tasks_.end())
                break;
            unindex_task(it->second);
            it->second.state = TaskState::Open;
            it->second.owner = 0;
            it->second.deadline_ms = 0;
            index_task(it->second);
            break;
        }
        case TaskEventKind::Update: {
            const auto it = tasks_.find(event.task_id);
            if (it == tasks_.end())
                break;
            it->second.flags = event.flags;
            break;
        }
    }
}

bool TaskBoard::persist(const TaskRecord& event) {
    // A board with no configured checkpoint is in-memory only (direct
    // Server tests); the host always configures one.
    if (checkpoint_ == nullptr)
        return !checkpoint_required_;
    if (std::fwrite(&event, sizeof(event), 1, checkpoint_) != 1 || std::fflush(checkpoint_) != 0)
        return false;
    ++stream_records_;
    return true;
}

bool TaskBoard::post(std::uint64_t actor, std::uint64_t parent, std::uint64_t ref,
                     std::uint64_t flags, Task* out, TaskRecord* event, std::string* why) {
    Task task{};
    task.id = next_id_++;
    task.parent = parent;
    task.ref = ref;
    task.flags = flags;
    task.state = TaskState::Open;
    TaskRecord record{};
    record.task_id = task.id;
    record.parent = parent;
    record.kind = static_cast<std::uint64_t>(TaskEventKind::Post);
    record.actor = actor;
    record.ref = ref;
    record.flags = flags;
    if (!persist(record)) {
        --next_id_;
        if (why)
            *why = "task checkpoint write failed";
        return false;
    }
    tasks_[task.id] = task;
    index_task(task);
    if (out)
        *out = task;
    if (event)
        *event = record;
    return true;
}

bool TaskBoard::claim(std::uint64_t actor, std::uint64_t task_id, std::uint64_t lease_ms,
                      std::uint64_t now_ms, Task* out, TaskRecord* event, std::string* why) {
    Task* task = nullptr;
    if (task_id != 0) {
        const auto it = tasks_.find(task_id);
        if (it == tasks_.end()) {
            if (why)
                *why = "task-unknown";
            return false;
        }
        task = &it->second;
    } else {
        if (open_ids_.empty()) {
            if (why)
                *why = "task-none-open";
            return false;
        }
        task = &tasks_.find(*open_ids_.begin())->second;
    }
    if (task->state != TaskState::Open) {
        if (why)
            *why = task_state_reason(task->state);
        return false;
    }
    if (lease_ms == 0)
        lease_ms = kTaskDefaultLeaseMs;
    TaskRecord record{};
    record.task_id = task->id;
    record.parent = task->parent;
    record.kind = static_cast<std::uint64_t>(TaskEventKind::Claim);
    record.actor = actor;
    record.lease_epoch = task->lease_epoch + 1;
    record.deadline_ms = now_ms + lease_ms;
    record.ref = task->ref;
    if (!persist(record)) {
        if (why)
            *why = "task checkpoint write failed";
        return false;
    }
    unindex_task(*task);
    task->state = TaskState::Claimed;
    task->owner = actor;
    task->lease_epoch = record.lease_epoch;
    task->deadline_ms = record.deadline_ms;
    index_task(*task);
    if (out)
        *out = *task;
    if (event)
        *event = record;
    return true;
}

bool TaskBoard::heartbeat(std::uint64_t actor, std::uint64_t task_id, std::uint64_t lease_ms,
                          std::uint64_t now_ms, Task* out, TaskRecord* event, std::string* why) {
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        if (why)
            *why = "task-unknown";
        return false;
    }
    Task& task = it->second;
    if (task.state != TaskState::Claimed) {
        if (why)
            *why = task_state_reason(task.state);
        return false;
    }
    if (task.owner != actor) {
        if (why)
            *why = "task-not-owner";
        return false;
    }
    if (task.deadline_ms != 0 && now_ms >= task.deadline_ms) {
        if (why)
            *why = "task-lease-expired";
        return false;
    }
    if (lease_ms == 0)
        lease_ms = kTaskDefaultLeaseMs;
    TaskRecord record{};
    record.task_id = task.id;
    record.parent = task.parent;
    record.kind = static_cast<std::uint64_t>(TaskEventKind::Heartbeat);
    record.actor = actor;
    record.lease_epoch = task.lease_epoch;
    record.deadline_ms = now_ms + lease_ms;
    record.ref = task.ref;
    if (!persist(record)) {
        if (why)
            *why = "task checkpoint write failed";
        return false;
    }
    unindex_task(task);
    task.deadline_ms = record.deadline_ms;
    index_task(task);
    if (out)
        *out = task;
    if (event)
        *event = record;
    return true;
}

bool TaskBoard::complete(std::uint64_t actor, std::uint64_t task_id, std::uint64_t ref,
                         std::uint64_t now_ms, Task* out, TaskRecord* event, std::string* why) {
    (void)now_ms;
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        if (why)
            *why = "task-unknown";
        return false;
    }
    Task& task = it->second;
    if (task.state != TaskState::Claimed) {
        if (why)
            *why = task_state_reason(task.state);
        return false;
    }
    if (task.owner != actor) {
        if (why)
            *why = "task-not-owner";
        return false;
    }
    TaskRecord record{};
    record.task_id = task.id;
    record.parent = task.parent;
    record.kind = static_cast<std::uint64_t>(TaskEventKind::Complete);
    record.actor = actor;
    record.lease_epoch = task.lease_epoch;
    record.ref = ref;
    if (!persist(record)) {
        if (why)
            *why = "task checkpoint write failed";
        return false;
    }
    unindex_task(task);
    task.state = TaskState::Done;
    task.ref = ref;
    task.deadline_ms = 0;
    terminal_order_.push_back(task.id);
    if (out)
        *out = task;
    if (event)
        *event = record;
    maybe_prune_terminal();
    return true;
}

bool TaskBoard::fail(std::uint64_t actor, std::uint64_t task_id, std::uint64_t ref,
                     std::uint64_t now_ms, Task* out, TaskRecord* event, std::string* why) {
    (void)now_ms;
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        if (why)
            *why = "task-unknown";
        return false;
    }
    Task& task = it->second;
    if (task.state != TaskState::Claimed) {
        if (why)
            *why = task_state_reason(task.state);
        return false;
    }
    if (task.owner != actor) {
        if (why)
            *why = "task-not-owner";
        return false;
    }
    TaskRecord record{};
    record.task_id = task.id;
    record.parent = task.parent;
    record.kind = static_cast<std::uint64_t>(TaskEventKind::Fail);
    record.actor = actor;
    record.lease_epoch = task.lease_epoch;
    record.ref = ref;
    if (!persist(record)) {
        if (why)
            *why = "task checkpoint write failed";
        return false;
    }
    unindex_task(task);
    task.state = TaskState::Failed;
    task.ref = ref;
    task.deadline_ms = 0;
    terminal_order_.push_back(task.id);
    if (out)
        *out = task;
    if (event)
        *event = record;
    maybe_prune_terminal();
    return true;
}

bool TaskBoard::release(std::uint64_t actor, std::uint64_t task_id, Task* out, TaskRecord* event,
                        std::string* why) {
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        if (why)
            *why = "task-unknown";
        return false;
    }
    Task& task = it->second;
    if (task.state != TaskState::Claimed) {
        if (why)
            *why = task_state_reason(task.state);
        return false;
    }
    if (task.owner != actor) {
        if (why)
            *why = "task-not-owner";
        return false;
    }
    TaskRecord record{};
    record.task_id = task.id;
    record.parent = task.parent;
    record.kind = static_cast<std::uint64_t>(TaskEventKind::Release);
    record.actor = actor;
    record.lease_epoch = task.lease_epoch;
    record.ref = task.ref;
    if (!persist(record)) {
        if (why)
            *why = "task checkpoint write failed";
        return false;
    }
    unindex_task(task);
    task.state = TaskState::Open;
    task.owner = 0;
    task.deadline_ms = 0;
    index_task(task);
    if (out)
        *out = task;
    if (event)
        *event = record;
    return true;
}

bool TaskBoard::set_flags(std::uint64_t task_id, std::uint64_t flags, Task* out, TaskRecord* event,
                          std::string* why) {
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
        if (why)
            *why = "task-unknown";
        return false;
    }
    Task& task = it->second;
    if (task.state != TaskState::Open) {
        if (why)
            *why = task_state_reason(task.state);
        return false;
    }
    TaskRecord record{};
    record.task_id = task.id;
    record.parent = task.parent;
    record.kind = static_cast<std::uint64_t>(TaskEventKind::Update);
    record.actor = 0;
    record.ref = task.ref;
    record.flags = flags;
    if (!persist(record)) {
        if (why)
            *why = "task checkpoint write failed";
        return false;
    }
    task.flags = flags;
    if (out)
        *out = task;
    if (event)
        *event = record;
    return true;
}

std::size_t TaskBoard::expire(std::uint64_t now_ms, std::vector<TaskRecord>* events) {
    std::size_t count = 0;
    while (!leases_.empty()) {
        const auto [deadline, task_id] = *leases_.begin();
        if (now_ms < deadline)
            break;
        leases_.erase(leases_.begin());
        const auto it = tasks_.find(task_id);
        if (it == tasks_.end())
            continue;
        Task& task = it->second;
        if (task.state != TaskState::Claimed || task.deadline_ms != deadline)
            continue;
        TaskRecord record{};
        record.task_id = task.id;
        record.parent = task.parent;
        record.kind = static_cast<std::uint64_t>(TaskEventKind::Expire);
        record.actor = task.owner;
        record.lease_epoch = task.lease_epoch;
        record.deadline_ms = task.deadline_ms;
        record.ref = task.ref;
        (void)persist(record);
        unindex_task(task);
        task.state = TaskState::Open;
        task.owner = 0;
        task.deadline_ms = 0;
        index_task(task);
        ++count;
        if (events)
            events->push_back(record);
    }
    return count;
}

bool TaskBoard::get(std::uint64_t id, Task* out) const {
    const auto it = tasks_.find(id);
    if (it == tasks_.end())
        return false;
    if (out)
        *out = it->second;
    return true;
}

bool TaskBoard::next_open(Task* out) const {
    if (open_ids_.empty())
        return false;
    const auto it = tasks_.find(*open_ids_.begin());
    if (it == tasks_.end())
        return false;
    if (out)
        *out = it->second;
    return true;
}

std::vector<Task> TaskBoard::open_tasks() const {
    std::vector<Task> out;
    out.reserve(open_ids_.size());
    for (const std::uint64_t id : open_ids_) {
        const auto it = tasks_.find(id);
        if (it != tasks_.end())
            out.push_back(it->second);
    }
    return out;
}

std::vector<Task> TaskBoard::claimed_tasks() const {
    std::vector<Task> out;
    out.reserve(claimed_ids_.size());
    for (const std::uint64_t id : claimed_ids_) {
        const auto it = tasks_.find(id);
        if (it != tasks_.end())
            out.push_back(it->second);
    }
    return out;
}

std::vector<Task> TaskBoard::list() const {
    std::vector<Task> out;
    out.reserve(tasks_.size());
    for (const auto& [id, task] : tasks_) {
        (void)id;
        out.push_back(task);
    }
    return out;
}

std::size_t TaskBoard::take_pruned(std::vector<std::uint64_t>* out) {
    if (pruned_ids_.empty())
        return 0;
    if (out != nullptr)
        out->insert(out->end(), pruned_ids_.begin(), pruned_ids_.end());
    const std::size_t count = pruned_ids_.size();
    pruned_ids_.clear();
    return count;
}

bool TaskBoard::take_compacted() {
    const bool flag = compacted_;
    compacted_ = false;
    return flag;
}

void TaskBoard::maybe_prune_terminal() {
    bool pruned = false;
    while (terminal_order_.size() > terminal_retention_) {
        const std::uint64_t id = terminal_order_.front();
        terminal_order_.pop_front();
        const auto it = tasks_.find(id);
        if (it == tasks_.end())
            continue;
        if (!is_terminal(it->second.state))
            continue;
        tasks_.erase(it);
        pruned_ids_.push_back(id);
        ++pruned_since_compact_;
        pruned = true;
    }
    if (!pruned)
        return;
    const std::size_t threshold = std::max<std::size_t>(64, tasks_.size());
    if (pruned_since_compact_ >= threshold)
        (void)compact_checkpoint();
}

bool TaskBoard::compact_checkpoint() {
    if (checkpoint_path_.empty()) {
        pruned_since_compact_ = 0;
        compacted_ = true;
        return true;
    }
    const std::string temp = checkpoint_path_ + ".compact";
    std::FILE* out = std::fopen(temp.c_str(), "w+b");
    if (out == nullptr)
        return false;
    TaskCheckpointHeaderV2 header{};
    std::memcpy(header.magic, kTaskCheckpointMagic, sizeof(header.magic));
    header.version = kTaskCheckpointVersion;
    header.count = tasks_.size();
    header.next_id = next_id_;
    bool ok = std::fwrite(&header, sizeof(header), 1, out) == 1;
    std::size_t records = 0;
    for (const auto& [id, task] : tasks_) {
        (void)id;
        TaskRecord post{};
        post.task_id = task.id;
        post.parent = task.parent;
        post.kind = static_cast<std::uint64_t>(TaskEventKind::Post);
        post.ref = task.ref;
        post.flags = task.flags;
        if (std::fwrite(&post, sizeof(post), 1, out) != 1) {
            ok = false;
            break;
        }
        ++records;
        if (task.state == TaskState::Open)
            continue;
        const TaskRecord state = state_record(task);
        if (std::fwrite(&state, sizeof(state), 1, out) != 1) {
            ok = false;
            break;
        }
        ++records;
    }
    if (ok && (std::fflush(out) != 0 || ::fsync(::fileno(out)) != 0))
        ok = false;
    if (std::fclose(out) != 0)
        ok = false;
    if (!ok) {
        (void)std::remove(temp.c_str());
        return false;
    }
    if (checkpoint_ != nullptr) {
        std::fclose(checkpoint_);
        checkpoint_ = nullptr;
    }
    if (std::rename(temp.c_str(), checkpoint_path_.c_str()) != 0) {
        (void)std::remove(temp.c_str());
        checkpoint_ = std::fopen(checkpoint_path_.c_str(), "r+b");
        if (checkpoint_ != nullptr)
            std::fseek(checkpoint_, 0, SEEK_END);
        return false;
    }
    checkpoint_ = std::fopen(checkpoint_path_.c_str(), "r+b");
    if (checkpoint_ == nullptr)
        return false;
    std::fseek(checkpoint_, 0, SEEK_END);
    stream_records_ = records;
    pruned_since_compact_ = 0;
    compacted_ = true;
    return true;
}

} // namespace aura::bus
