// parallel_orch.h — Issue #1586 / #1202: parallel agent orchestration.
// Header form (like multi_fiber_mailbox.h) so serve + tests can include
// without module churn. Spawns fibers under a concurrency cap, joins via
// Fiber::join (#1584), optional MultiFiberMailbox result routing (#1585).
// #1595: join Ok bumps linear enforcement counters; host refresh via
// complete_post_join_linear_enforcement.

#ifndef AURA_SERVE_PARALLEL_ORCH_H
#define AURA_SERVE_PARALLEL_ORCH_H

#include "fiber.h"
#include "multi_fiber_mailbox.h"
#include "scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace aura::serve::parallel_orch {

inline constexpr int kParallelOrchPhase = 2; // #1586 production
inline constexpr int kParallelOrchIssue = 1586;

// ── Policy ─────────────────────────────────────────────
struct ParallelPolicy {
    std::uint32_t max_concurrency = 8; // 1..1024
    std::uint32_t timeout_ms = 0;      // 0 = no overall deadline
    bool fail_fast = false;            // stop admitting work after first error
    bool collect_errors = true;        // keep err results (vs only ok)
};

// ── Task I/O ───────────────────────────────────────────
struct TaskResult {
    bool ok = true;
    std::string value; // opaque payload / agent result
    std::string error; // non-empty when !ok
    std::uint64_t task_index = 0;
    std::uint64_t fiber_id = 0;
    std::uint64_t elapsed_us = 0;
};

struct TaskSpec {
    std::function<TaskResult()> body; // required
    std::string name;                 // optional label
};

enum class BatchStatus : std::uint8_t {
    Ok = 0,       // all tasks succeeded
    Partial = 1,  // some errors, completed without fail-fast abort
    Timeout = 2,  // overall join deadline hit
    FailFast = 3, // aborted remaining after first error
    Invalid = 4,  // bad policy / empty / null scheduler
};

struct BatchResult {
    BatchStatus status = BatchStatus::Invalid;
    std::vector<TaskResult> results;
    std::uint64_t wait_us = 0;
    std::uint32_t ok_count = 0;
    std::uint32_t err_count = 0;
    std::uint32_t aborted_count = 0;
    JoinStatus join_status = JoinStatus::Ok;
};

// ── Process-wide stats ─────────────────────────────────
struct ParallelOrchStats {
    std::atomic<std::uint64_t> intend_batches{0};
    std::atomic<std::uint64_t> tasks_spawned{0};
    std::atomic<std::uint64_t> tasks_joined{0};
    std::atomic<std::uint64_t> tasks_ok{0};
    std::atomic<std::uint64_t> tasks_err{0};
    std::atomic<std::uint64_t> fail_fast_aborts{0};
    std::atomic<std::uint64_t> timeouts{0};
    std::atomic<std::uint64_t> invalid_batches{0};
    std::atomic<std::uint64_t> mailbox_posts{0};
    std::atomic<std::uint64_t> sequential_baseline_us{0};
    std::atomic<std::uint64_t> parallel_elapsed_us{0};
};

inline ParallelOrchStats g_parallel_orch_stats{};

[[nodiscard]] inline bool validate_policy(const ParallelPolicy& p) noexcept {
    return p.max_concurrency > 0 && p.max_concurrency <= 1024;
}

inline void snapshot_global(std::uint64_t& batches, std::uint64_t& spawned, std::uint64_t& joined,
                            std::uint64_t& ok, std::uint64_t& err, std::uint64_t& fail_fast,
                            std::uint64_t& timeouts, std::uint64_t& mailbox_posts) noexcept {
    batches = g_parallel_orch_stats.intend_batches.load(std::memory_order_relaxed);
    spawned = g_parallel_orch_stats.tasks_spawned.load(std::memory_order_relaxed);
    joined = g_parallel_orch_stats.tasks_joined.load(std::memory_order_relaxed);
    ok = g_parallel_orch_stats.tasks_ok.load(std::memory_order_relaxed);
    err = g_parallel_orch_stats.tasks_err.load(std::memory_order_relaxed);
    fail_fast = g_parallel_orch_stats.fail_fast_aborts.load(std::memory_order_relaxed);
    timeouts = g_parallel_orch_stats.timeouts.load(std::memory_order_relaxed);
    mailbox_posts = g_parallel_orch_stats.mailbox_posts.load(std::memory_order_relaxed);
}

// ── Core: parallel_run ─────────────────────────────────
// Spawns one fiber per task under max_concurrency (internal permit gate).
// Joins all via Fiber::join(span). Optionally posts each result to `mb`
// for multi-agent consumers (MultiFiberMailbox integration).
//
// Task bodies must not throw across the fiber boundary in production; any
// exception is caught and turned into TaskResult{ok=false,error=...}.
[[nodiscard]] inline BatchResult parallel_run(Scheduler& sched, std::span<const TaskSpec> tasks,
                                              ParallelPolicy policy = {},
                                              mf_mailbox::MultiFiberMailbox* mb = nullptr) {
    BatchResult out;
    if (!validate_policy(policy)) {
        g_parallel_orch_stats.invalid_batches.fetch_add(1, std::memory_order_relaxed);
        out.status = BatchStatus::Invalid;
        return out;
    }
    if (tasks.empty()) {
        out.status = BatchStatus::Ok;
        return out;
    }

    const auto t0 = std::chrono::steady_clock::now();
    g_parallel_orch_stats.intend_batches.fetch_add(1, std::memory_order_relaxed);
    g_parallel_orch_stats.tasks_spawned.fetch_add(tasks.size(), std::memory_order_relaxed);

    // Shared so timed-out join does not leave fibers writing into stack out.
    struct Shared {
        std::mutex results_mu;
        std::vector<TaskResult> results;
        std::atomic<std::uint32_t> active{0};
        std::atomic<bool> abort_admit{false};
        std::atomic<std::uint32_t> ok_n{0};
        std::atomic<std::uint32_t> err_n{0};
        std::atomic<std::uint32_t> aborted_n{0};
    };
    auto sh = std::make_shared<Shared>();
    sh->results.resize(tasks.size());
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        sh->results[i].task_index = i;
        sh->results[i].ok = false;
        sh->results[i].error = "not-started";
    }

    std::vector<Fiber*> fibers;
    fibers.reserve(tasks.size());
    const auto max_c = policy.max_concurrency;
    const bool fail_fast = policy.fail_fast;

    for (std::size_t i = 0; i < tasks.size(); ++i) {
        auto body = tasks[i].body;
        const std::string name = tasks[i].name;
        Fiber* f = sched.spawn([sh, i, body = std::move(body), name, max_c, fail_fast,
                                mb]() mutable {
            // Concurrency gate: yield until a permit is free (steal-friendly).
            for (;;) {
                if (sh->abort_admit.load(std::memory_order_acquire) ||
                    (g_current_fiber && g_current_fiber->is_cancel_requested())) {
                    TaskResult aborted;
                    aborted.ok = false;
                    aborted.error = "aborted:fail-fast";
                    aborted.task_index = i;
                    if (g_current_fiber)
                        aborted.fiber_id = g_current_fiber->id();
                    {
                        std::lock_guard lock(sh->results_mu);
                        if (i < sh->results.size())
                            sh->results[i] = std::move(aborted);
                    }
                    sh->aborted_n.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                auto cur = sh->active.load(std::memory_order_relaxed);
                if (cur < max_c) {
                    if (sh->active.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed))
                        break;
                } else {
                    Fiber::yield(YieldReason::Explicit);
                }
            }

            std::uint64_t fiber_id = 0;
            if (g_current_fiber)
                fiber_id = g_current_fiber->id();

            const auto t_task = std::chrono::steady_clock::now();
            TaskResult r;
            try {
                if (body)
                    r = body();
                else {
                    r.ok = false;
                    r.error = "empty-body";
                }
            } catch (const std::exception& ex) {
                r.ok = false;
                r.error = ex.what();
            } catch (...) {
                r.ok = false;
                r.error = "unknown-exception";
            }
            r.task_index = i;
            r.fiber_id = fiber_id;
            r.elapsed_us =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - t_task)
                                               .count());
            if (!r.ok && r.error.empty())
                r.error = name.empty() ? "task-failed" : ("task-failed:" + name);

            const bool ok = r.ok;
            const std::string value = r.value;
            const std::string error = r.error;
            {
                std::lock_guard lock(sh->results_mu);
                if (i < sh->results.size())
                    sh->results[i] = std::move(r);
            }

            if (ok) {
                sh->ok_n.fetch_add(1, std::memory_order_relaxed);
                g_parallel_orch_stats.tasks_ok.fetch_add(1, std::memory_order_relaxed);
            } else {
                sh->err_n.fetch_add(1, std::memory_order_relaxed);
                g_parallel_orch_stats.tasks_err.fetch_add(1, std::memory_order_relaxed);
                if (fail_fast) {
                    sh->abort_admit.store(true, std::memory_order_release);
                    g_parallel_orch_stats.fail_fast_aborts.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (mb) {
                mf_mailbox::MailMessage msg;
                msg.from_fiber = fiber_id;
                msg.to_fiber = 0;
                msg.priority =
                    ok ? mf_mailbox::MailPriority::Normal : mf_mailbox::MailPriority::High;
                msg.payload = ok ? value : (std::string("err:") + error);
                if (mb->push(std::move(msg)) == mf_mailbox::PushStatus::Ok)
                    g_parallel_orch_stats.mailbox_posts.fetch_add(1, std::memory_order_relaxed);
            }

            sh->active.fetch_sub(1, std::memory_order_acq_rel);
        });
        fibers.push_back(f);
    }

    std::optional<std::uint64_t> join_timeout;
    if (policy.timeout_ms > 0)
        join_timeout = static_cast<std::uint64_t>(policy.timeout_ms);

    // Fiber::join Ok path bumps join_linear_enforcement_total per target (#1595).
    JoinResult jr = Fiber::join(std::span<Fiber* const>(fibers), join_timeout);
    out.join_status = jr.status;
    out.wait_us = jr.wait_us;
    g_parallel_orch_stats.tasks_joined.fetch_add(fibers.size(), std::memory_order_relaxed);

    // On overall timeout: stop admitting + request cancel, then best-effort drain.
    // Task bodies should check Fiber::is_cancel_requested() / not spin forever.
    if (jr.status == JoinStatus::Timeout) {
        g_parallel_orch_stats.timeouts.fetch_add(1, std::memory_order_relaxed);
        sh->abort_admit.store(true, std::memory_order_release);
        for (auto* f : fibers) {
            if (f)
                f->request_cancel();
        }
        (void)Fiber::join(std::span<Fiber* const>(fibers), std::optional<std::uint64_t>{2000});
    }

    // Mutex-protected snapshot; shared_ptr keeps storage alive for any late fiber.
    {
        std::lock_guard lock(sh->results_mu);
        out.results = sh->results;
    }
    out.ok_count = sh->ok_n.load(std::memory_order_relaxed);
    out.err_count = sh->err_n.load(std::memory_order_relaxed);
    out.aborted_count = sh->aborted_n.load(std::memory_order_relaxed);

    if (!policy.collect_errors) {
        for (auto& r : out.results) {
            if (!r.ok) {
                r.error.clear();
                r.value.clear();
            }
        }
    }

    if (jr.status == JoinStatus::Timeout) {
        out.status = BatchStatus::Timeout;
    } else if (out.aborted_count > 0 || (policy.fail_fast && out.err_count > 0)) {
        out.status = BatchStatus::FailFast;
    } else if (out.err_count > 0) {
        out.status = BatchStatus::Partial;
    } else {
        out.status = BatchStatus::Ok;
    }

    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    g_parallel_orch_stats.parallel_elapsed_us.fetch_add(elapsed, std::memory_order_relaxed);
    return out;
}

// Agent-facing alias (issue title: parallel_intend).
[[nodiscard]] inline BatchResult parallel_intend(Scheduler& sched, std::span<const TaskSpec> tasks,
                                                 ParallelPolicy policy = {},
                                                 mf_mailbox::MultiFiberMailbox* mb = nullptr) {
    return parallel_run(sched, tasks, policy, mb);
}

// Sequential baseline for throughput comparisons (same TaskSpec bodies).
[[nodiscard]] inline BatchResult sequential_run(std::span<const TaskSpec> tasks) {
    BatchResult out;
    out.results.resize(tasks.size());
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        TaskResult r;
        try {
            if (tasks[i].body)
                r = tasks[i].body();
            else {
                r.ok = false;
                r.error = "empty-body";
            }
        } catch (const std::exception& ex) {
            r.ok = false;
            r.error = ex.what();
        } catch (...) {
            r.ok = false;
            r.error = "unknown-exception";
        }
        r.task_index = i;
        out.results[i] = std::move(r);
        if (out.results[i].ok)
            ++out.ok_count;
        else
            ++out.err_count;
    }
    out.wait_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    g_parallel_orch_stats.sequential_baseline_us.fetch_add(out.wait_us, std::memory_order_relaxed);
    out.status = out.err_count == 0 ? BatchStatus::Ok : BatchStatus::Partial;
    out.join_status = JoinStatus::Ok;
    return out;
}

} // namespace aura::serve::parallel_orch

#endif // AURA_SERVE_PARALLEL_ORCH_H
