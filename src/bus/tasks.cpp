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
    constexpr std::uint32_t kTaskCheckpointVersion = 1;
    constexpr std::uint64_t kTaskDefaultLeaseMs = 30000;

    struct TaskCheckpointHeader {
        char magic[8];
        std::uint32_t version;
        std::uint32_t reserved;
        std::uint64_t count;
    };
    static_assert(sizeof(TaskCheckpointHeader) == 24);

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

} // namespace

TaskBoard::~TaskBoard() {
    if (checkpoint_ != nullptr)
        std::fclose(checkpoint_);
}

bool TaskBoard::resume(const std::string& path, std::string* why) {
    checkpoint_path_ = path;
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
        TaskCheckpointHeader header{};
        std::memcpy(header.magic, kTaskCheckpointMagic, sizeof(header.magic));
        header.version = kTaskCheckpointVersion;
        header.count = 0;
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
    if (size == 0) {
        // A crash between create and header write must not wedge every
        // later host: an empty file is a fresh board.
        TaskCheckpointHeader header{};
        std::memcpy(header.magic, kTaskCheckpointMagic, sizeof(header.magic));
        header.version = kTaskCheckpointVersion;
        if (std::fwrite(&header, sizeof(header), 1, f) != 1) {
            if (why)
                *why = "task checkpoint header write failed";
            std::fclose(f);
            return false;
        }
        std::fflush(f);
    } else {
        TaskCheckpointHeader header{};
        if (std::fread(&header, sizeof(header), 1, f) != 1 ||
            std::memcmp(header.magic, kTaskCheckpointMagic, sizeof(header.magic)) != 0 ||
            header.version != kTaskCheckpointVersion) {
            if (why)
                *why = "task checkpoint is malformed or from another version";
            std::fclose(f);
            return false;
        }
    }
    // The record stream, not the informational count, is authoritative.
    // A torn tail (a crash mid-append) is dropped so the next append
    // starts a whole record instead of gluing two partial events.
    long valid_end = static_cast<long>(sizeof(TaskCheckpointHeader));
    while (true) {
        TaskRecord record{};
        const std::size_t n = std::fread(&record, sizeof(record), 1, f);
        if (n != 1)
            break;
        apply(record);
        valid_end = std::ftell(f);
    }
    if (valid_end < 0 || ::ftruncate(::fileno(f), valid_end) != 0) {
        if (why)
            *why = "task checkpoint truncate failed";
        std::fclose(f);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    checkpoint_ = f;
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
            task.state = TaskState::Open;
            tasks_[task.id] = task;
            next_id_ = std::max(next_id_, task.id + 1);
            break;
        }
        case TaskEventKind::Claim: {
            const auto it = tasks_.find(event.task_id);
            if (it == tasks_.end())
                break;
            it->second.state = TaskState::Claimed;
            it->second.owner = event.actor;
            it->second.lease_epoch = event.lease_epoch;
            it->second.deadline_ms = event.deadline_ms;
            break;
        }
        case TaskEventKind::Heartbeat: {
            const auto it = tasks_.find(event.task_id);
            if (it == tasks_.end() || it->second.state != TaskState::Claimed)
                break;
            if (event.lease_epoch != 0 && event.lease_epoch != it->second.lease_epoch)
                break;
            it->second.deadline_ms = event.deadline_ms;
            break;
        }
        case TaskEventKind::Complete:
        case TaskEventKind::Fail: {
            const auto it = tasks_.find(event.task_id);
            if (it == tasks_.end())
                break;
            it->second.state = static_cast<TaskEventKind>(event.kind) == TaskEventKind::Complete
                                   ? TaskState::Done
                                   : TaskState::Failed;
            it->second.owner = event.actor;
            it->second.ref = event.ref;
            it->second.deadline_ms = 0;
            break;
        }
        case TaskEventKind::Release:
        case TaskEventKind::Expire:
        case TaskEventKind::Requeue: {
            const auto it = tasks_.find(event.task_id);
            if (it == tasks_.end())
                break;
            it->second.state = TaskState::Open;
            it->second.owner = 0;
            it->second.deadline_ms = 0;
            break;
        }
    }
}

bool TaskBoard::persist(const TaskRecord& event) {
    // A board with no configured checkpoint is in-memory only (direct
    // Server tests); the host always configures one.
    if (checkpoint_ == nullptr)
        return true;
    if (std::fwrite(&event, sizeof(event), 1, checkpoint_) != 1 || std::fflush(checkpoint_) != 0)
        return false;
    return true;
}

bool TaskBoard::post(std::uint64_t actor, std::uint64_t parent, std::uint64_t ref, Task* out,
                     TaskRecord* event, std::string* why) {
    Task task{};
    task.id = next_id_++;
    task.parent = parent;
    task.ref = ref;
    task.state = TaskState::Open;
    TaskRecord record{};
    record.task_id = task.id;
    record.parent = parent;
    record.kind = static_cast<std::uint64_t>(TaskEventKind::Post);
    record.actor = actor;
    record.ref = ref;
    if (!persist(record)) {
        --next_id_;
        if (why)
            *why = "task checkpoint write failed";
        return false;
    }
    tasks_[task.id] = task;
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
        for (auto& [id, candidate] : tasks_) {
            (void)id;
            if (candidate.state == TaskState::Open) {
                task = &candidate;
                break;
            }
        }
        if (task == nullptr) {
            if (why)
                *why = "task-none-open";
            return false;
        }
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
    task->state = TaskState::Claimed;
    task->owner = actor;
    task->lease_epoch = record.lease_epoch;
    task->deadline_ms = record.deadline_ms;
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
    task.deadline_ms = record.deadline_ms;
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
    task.state = TaskState::Done;
    task.ref = ref;
    task.deadline_ms = 0;
    if (out)
        *out = task;
    if (event)
        *event = record;
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
    task.state = TaskState::Failed;
    task.ref = ref;
    task.deadline_ms = 0;
    if (out)
        *out = task;
    if (event)
        *event = record;
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
    task.state = TaskState::Open;
    task.owner = 0;
    task.deadline_ms = 0;
    if (out)
        *out = task;
    if (event)
        *event = record;
    return true;
}

std::size_t TaskBoard::expire(std::uint64_t now_ms, std::vector<TaskRecord>* events) {
    std::size_t count = 0;
    for (auto& [id, task] : tasks_) {
        (void)id;
        if (task.state != TaskState::Claimed || task.deadline_ms == 0 || now_ms < task.deadline_ms)
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
        task.state = TaskState::Open;
        task.owner = 0;
        task.deadline_ms = 0;
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

std::vector<Task> TaskBoard::list() const {
    std::vector<Task> out;
    out.reserve(tasks_.size());
    for (const auto& [id, task] : tasks_) {
        (void)id;
        out.push_back(task);
    }
    return out;
}

bool TaskBoard::has_leases() const {
    for (const auto& [id, task] : tasks_) {
        (void)id;
        if (task.state == TaskState::Claimed)
            return true;
    }
    return false;
}

} // namespace aura::bus
