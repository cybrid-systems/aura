// serve/serve_async.cpp — Async serve mode implementation
#include "serve_async.h"
#include "messaging_bridge.h"
#include "scheduler.h"
#include "fiber.h"
#include "mailbox.h"
#include "thread_pool.h"

#include <print>
#include <iostream>
#include <thread>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <array>
#include <sys/epoll.h>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string>
#include <poll.h>
#include <deque>
#include <dlfcn.h>
#include <unordered_map>
#include <memory>

import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace aura::serve {

// ── Helpers ─────────────────────────────────────────────

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (auto c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c;
        }
    }
    return out;
}

// Minimal JSON field extractor (no full parser needed for protocol)
static std::string json_field(const std::string& json, const std::string& field) {
    // Try with space after colon, then without
    auto key = "\"" + field + "\": \"";
    auto pos = json.find(key);
    if (pos == std::string::npos) {
        key = "\"" + field + "\":\"";
        pos = json.find(key);
    }
    if (pos == std::string::npos) return {};
    pos += key.size();
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            auto next = json[pos + 1];
            if (next == 'n') val += '\n';
            else if (next == 't') val += '\t';
            else if (next == 'r') val += '\r';
            else if (next == '"') val += '"';
            else if (next == '\\') val += '\\';
            else val += next;
            pos += 2;
        } else {
            val += json[pos++];
        }
    }
    return val;
}

// ── Value formatting ─────────────────────────────────────

using EvalValue = aura::compiler::types::EvalValue;
using aura::compiler::types::is_closure;

static std::string fmt_val(const EvalValue& v,
                            aura::compiler::CompilerService& cs) {
    return aura::compiler::format_value(v, &cs.evaluator().primitives().string_heap(),
                                         &cs.evaluator().pairs(), 0, &cs.evaluator().primitives(),
                                         &cs.evaluator().keyword_table());
}

// ── run_serve_async ─────────────────────────────────────

void run_serve_async(int num_workers) {
    // 1. Set stdin to non-blocking
    int flags = ::fcntl(STDIN_FILENO, F_GETFL);
    ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    // Register fiber blocking callback for CompilerService::pop_message
    aura::messaging::g_fiber_block = []() {
        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
    };

    // 2. Create thread pool for blocking operations
    // Background threads handle compilation, type-checking, file I/O,
    // and any other blocking tasks without blocking the event loop.
    // Pool size = num CPUs is a reasonable default.
    unsigned pool_size = std::thread::hardware_concurrency();
    if (pool_size < 2) pool_size = 2;
    if (pool_size > 8) pool_size = 8;
    static aura::serve::ThreadPool s_thread_pool(pool_size);

    // Register thread pool enqueue callback:
    // Injects the current fiber's eventfd as the wakeup mechanism.
    aura::messaging::g_thread_pool_enqueue = [](std::function<void()> fn, int) {
        auto* fiber = aura::serve::g_current_fiber;
        if (!fiber) return;
        int evfd = fiber->eventfd();
        if (evfd < 0) return;
        s_thread_pool.enqueue(std::move(fn), evfd);
    };

    // Register async eval callback: evaluates code on the thread pool.
    // Returns the result as a JSON-escaped string. Uses the current
    // session's CompilerService (only safe because the service's evaluator
    // state is captured in the closure and executed FIFO on the pool).
    aura::messaging::g_eval_async = [](const std::string& code) -> std::string {
        auto* fiber = aura::serve::g_current_fiber;
        if (!fiber) return "";
        auto evfd = fiber->eventfd();
        if (evfd < 0) return "";
        // Capture current compiler service for eval
        auto* svc = static_cast<aura::compiler::CompilerService*>(
            aura::messaging::g_current_compiler_service);
        if (!svc) return "";
        std::string result;
        s_thread_pool.enqueue([svc, code, &result]() {
            auto r = svc->exec_with_cache(code);
            if (r) {
                result = aura::compiler::format_value(
                    *r, &svc->evaluator().primitives().string_heap(),
                    &svc->evaluator().pairs(), 0,
                    &svc->evaluator().primitives(),
                    &svc->evaluator().keyword_table());
            } else {
                result = "[error] " + r.error().format();
            }
        }, evfd);
        // Yield and wait for completion
        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
        // Drain eventfd
        uint64_t val;
        ::read(evfd, &val, sizeof(val));
        return result;
    };

    // 2. Create scheduler with worker threads
    Scheduler sched(num_workers);

    // Register fiber:spawn callback (captures scheduler for actual fiber creation)
    aura::messaging::g_fiber_spawn = [&sched](std::function<void()> fn) -> int64_t {
        auto* f = sched.spawn(std::move(fn));
        return static_cast<int64_t>(f ? f->id() : 0);
    };

    // Register fiber:yield callback (non-blocking yield, fiber stays Ready)
    aura::messaging::g_fiber_yield = []() {
        if (aura::serve::g_current_fiber) {
            aura::serve::Fiber::yield();
        }
    };

    // 3. Shared state between stdin_reader and session fibers
    std::deque<std::string> stdin_lines;  // complete JSON lines from stdin
    bool stdin_eof = false;

    // 4. Spawn stdin_reader fiber

    auto* stdin_fiber = sched.spawn([&sched, &stdin_lines, &stdin_eof]() {

        std::string buf;
        bool local_eof = false;
        while (!local_eof) {
            // Edge-triggered: read until EAGAIN or EOF
            bool got_data = false;
            while (true) {
                char tmp[4096];
                ssize_t n = ::read(STDIN_FILENO, tmp, sizeof(tmp));
                if (n > 0) {
                    got_data = true;
                    buf.append(tmp, static_cast<size_t>(n));
                } else if (n == 0) {
                    local_eof = true;
                    break;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  // no more data now
                } else {
                    local_eof = true;
                    break;
                }
            }

            // Extract complete lines from buffer (do this BEFORE EOF check)
            auto nl = buf.find('\n');
            while (nl != std::string::npos) {
                auto line = buf.substr(0, nl);
                // Skip comment, blank, and empty lines
                auto s = line.find_first_not_of(" \t\r\n");
                if (s != std::string::npos && line[s] != ';')
                    stdin_lines.push_back(std::move(line));
                buf.erase(0, nl + 1);
                nl = buf.find('\n');
            }

            if (local_eof) {
                // EOF — remove stdin from epoll so it doesn't keep firing
                ::epoll_ctl(sched.epoll_fd(), EPOLL_CTL_DEL, STDIN_FILENO, nullptr);
                stdin_eof = true;
                break;
            }

            if (!got_data) {
                // Yield and wait for epoll to wake us on new data
                g_current_fiber->set_state(FiberState::Waiting);
                Fiber::yield();
            }
            // If got_data == true, loop back to try reading more
            // (allows consuming chained pipe data in one quantum)
        }
    });
    sched.set_stdin_fiber(stdin_fiber);

    // 5. Create default session with compiler service
    struct Session {
        std::string id;
        Fiber* fiber = nullptr;
        aura::compiler::CompilerService service;
        aura::serve::Mailbox mailbox;
        bool active = true;
    };

    // Shared workspace tree: all sessions see the same workspace hierarchy.
    // Created before any sessions so we can inject it into each one.
    void* shared_workspace_tree = aura::compiler::Evaluator::create_workspace_tree();

    std::unordered_map<std::string, std::unique_ptr<Session>> sessions;
    std::string active_session = "default";
    {
        auto& sess = sessions["default"];
        sess = std::make_unique<Session>();
        sess->id = "default";
        sess->service.set_session_id("default");
        sess->service.set_workspace_tree(shared_workspace_tree);
        aura::compiler::CompilerService::register_session("default", &sess->service);
    }

    // Register async HTTP handler (uses thread + fork+exec curl + pipe with drain)
    // Thread does blocking fork+exec; pipe read has a 30s timeout to avoid deadlock.
    aura::messaging::g_http_post_async = [](const std::string& url,
                                              const std::string& body,
                                              const std::string& auth) -> std::string {
        auto* fiber = aura::serve::g_current_fiber;
        if (!fiber) return {};
        auto evfd = fiber->eventfd();
        if (evfd < 0) return {};

        std::string result;
        std::thread t([evfd, url, body, auth, &result]() {
            int in[2], out[2];
            if (::pipe(in) < 0 || ::pipe(out) < 0) {
                uint64_t v = 1; ::write(evfd, &v, sizeof(v)); return;
            }

            pid_t pid = ::fork();
            if (pid < 0) {
                ::close(in[0]); ::close(in[1]); ::close(out[0]); ::close(out[1]);
                uint64_t v = 1; ::write(evfd, &v, sizeof(v)); return;
            }

            if (pid == 0) {
                ::close(in[1]); ::close(out[0]);
                ::dup2(in[0], STDIN_FILENO); ::dup2(out[1], STDOUT_FILENO);
                ::close(in[0]); ::close(out[1]);

                // Build argv
                std::vector<const char*> argv;
                argv.push_back("curl");
                argv.push_back("-s"); argv.push_back("-X"); argv.push_back("POST");
                argv.push_back("--data-binary"); argv.push_back("@-");
                argv.push_back("-H"); argv.push_back("Content-Type: application/json");
                if (!auth.empty()) {
                    auto auth_hdr = "Authorization: Bearer " + auth;
                    argv.push_back("-H");
                    argv.push_back(auth_hdr.c_str());
                }
                argv.push_back("--max-time"); argv.push_back("30");
                argv.push_back("--connect-timeout"); argv.push_back("10");
                argv.push_back(url.c_str());
                argv.push_back(nullptr);

                ::execvp("curl", const_cast<char* const*>(argv.data()));
                ::_exit(1);
            }

            // Parent: send body, close stdin pipe
            ::close(in[0]); ::close(out[1]);
            ::write(in[1], body.data(), body.size()); ::close(in[1]);

            // Read response with timeout to prevent pipe deadlock
            // Large LLM responses (>64KB pipe buffer) can block child write
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
            std::array<char, 65536> fbuf;  // Large buffer to drain pipe fast
            struct pollfd pfd = {out[0], POLLIN, 0};
            ssize_t nr;
            while (true) {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0) break;
                int pr = ::poll(&pfd, 1, static_cast<int>(remaining));
                if (pr <= 0) break;
                nr = ::read(out[0], fbuf.data(), fbuf.size());
                if (nr <= 0) break;
                result.append(fbuf.data(), static_cast<std::size_t>(nr));
            }
            ::close(out[0]);

            int cstat;
            ::waitpid(pid, &cstat, 0);

            uint64_t v = 1;
            ::write(evfd, &v, sizeof(v));
        });
        t.detach();

        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();

        return result;
    };

    // Register session:create for Aura code (primitive in evaluator)
    std::function<aura::messaging::SessionCreateFn> sc_fn =
        [&sessions, &sched, shared_workspace_tree, &stdin_lines, &stdin_eof](const std::string& name) -> bool {
        if (sessions.count(name) > 0) return false;
        auto [it, created] = sessions.try_emplace(name, std::make_unique<Session>());
        if (!created) return false;
        it->second->id = name;
        it->second->service.set_session_id(name);
        it->second->service.set_workspace_tree(shared_workspace_tree);
        aura::compiler::CompilerService::register_session(name, &it->second->service);
        // Spawn a fiber so the session can process commands from stdin_lines
        auto nsid = name;
        auto* nf = sched.spawn([nsid, &sess = *it->second, &stdin_lines, &stdin_eof]() {
            sess.mailbox.attach(aura::serve::g_current_fiber);
            sess.service.set_wake_eventfd(aura::serve::g_current_fiber->eventfd());
            while (sess.active) {
                std::string sl;
                for (auto sit = stdin_lines.begin(); sit != stdin_lines.end(); ++sit) {
                    if (sit->find("\"session\":\"" + nsid + "\"") != std::string::npos) {
                        sl = std::move(*sit);
                        stdin_lines.erase(sit);
                        break;
                    }
                }
                if (sl.empty()) {
                    if (stdin_eof) break;
                    aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
                    aura::serve::Fiber::yield();
                    continue;
                }
                auto c = json_field(sl, "cmd");
                if (c == "exec") {
                    auto code = json_field(sl, "code");
                    if (!code.empty()) {
                        auto r = sess.service.exec_with_cache(code);
                        if (r) {
                            std::println("{{\"session\":\"{}\" ,\"status\":\"ok\",\"value\":\"{}\" }}",
                                         json_escape(nsid), json_escape(fmt_val(*r, sess.service)));
                        } else {
                            std::println("{{\"session\":\"{}\" ,\"status\":\"error\",\"msg\":\"{}\" }}",
                                         json_escape(nsid), json_escape(r.error().format()));
                        }
                    }
                }
                std::fflush(stdout);
            }
        });
        it->second->fiber = nf;
        return true;
    };
    aura::messaging::g_session_create = &sc_fn;

    // 6. Spawn session fibers (one per session)
    // For now, spawn one fiber for the default session
    // In the future, spawn as needed
    for (auto& [sid, sess] : sessions) {
        auto* fiber = sched.spawn([sid = sid, &sess = *sess, &stdin_lines, &stdin_eof, &sessions, &sched, &shared_workspace_tree]() {
            // Attach mailbox to this fiber
            sess.mailbox.attach(g_current_fiber);

            while (sess.active) {
                // Try to pop a line from stdin
                std::string line;
                {
                    // Lock-free: dequeue from shared buffer if available
                    // Since we're single-threaded, no actual lock needed
                    line.clear();
                    for (auto it = stdin_lines.begin(); it != stdin_lines.end(); ++it) {
                        // Check if this line is for our session
                        // Lines without "session" field go to "default"
                        bool for_me = false;
                        if (sid == "default") {
                            for_me = (it->find("\"session\":\"") == std::string::npos) ||
                                     (it->find("\"session\":\"default\"") != std::string::npos);
                        } else {
                            for_me = (it->find("\"session\":\"" + sid + "\"") != std::string::npos);
                        }
                        if (for_me) {
                            line = std::move(*it);
                            stdin_lines.erase(it);
                            break;
                        }
                    }
                }

                if (line.empty()) {
                    // No lines for us — check mailbox
                    auto msg = sess.mailbox.pop(false);
                    if (!msg.empty()) {
                        // Got a message from another session
                        std::println("{{\"session\":\"{}\",\"status\":\"msg\",\"data\":\"{}\"}}",
                                     json_escape(sid), json_escape(msg));
                        continue;
                    }

                    // Nothing to do
                    if (stdin_eof) break;
                    g_current_fiber->set_state(FiberState::Waiting);
                    Fiber::yield();
                    continue;
                }

                // Parse and execute
                auto cmd = json_field(line, "cmd");
                if (cmd.empty()) {
                    std::println("{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"missing cmd\"}}",
                                 json_escape(sid));
                    continue;
                }

                if (cmd == "exec") {
                    auto code = json_field(line, "code");
                    if (code.empty()) {
                        std::println("{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"missing code\"}}",
                                     json_escape(sid));
                        continue;
                    }
                    auto result = sess.service.exec_with_cache(code);
                    if (result) {
                        try {
                            auto& v = *result;
                            // Check if closure
                            if (is_closure(v)) {
                                std::println("{{\"session\":\"{}\",\"status\":\"closure\",\"value\":\"#<procedure>\"}}",
                                             json_escape(sid));
                            } else {
                                std::println("{{\"session\":\"{}\",\"status\":\"ok\",\"value\":\"{}\"}}",
                                             json_escape(sid), json_escape(fmt_val(v, sess.service)));
                            }
                        } catch (const std::bad_alloc&) {
                            std::println("{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"out of memory\"}}",
                                         json_escape(sid));
                        }
                    } else {
                        auto& d = result.error();
                        std::println("{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"{}\"}}",
                                     json_escape(sid), json_escape(d.format()));
                        std::fflush(stdout);
                    }

                } else if (cmd == "session") {
                    auto action = json_field(line, "action");
                    if (action == "create") {
                        auto name = json_field(line, "name");
                        if (name.empty()) {
                            std::println("{{\"session\":\"{}\" ,\"status\":\"error\",\"msg\":\"missing name\"}}",
                                         json_escape(sid));
                        } else {
                            auto [it, created] = sessions.try_emplace(name, std::make_unique<Session>());
                            if (created) {
                                it->second->id = name;
                                it->second->service.set_session_id(name);
                                it->second->service.set_workspace_tree(shared_workspace_tree);
                                aura::compiler::CompilerService::register_session(name, &it->second->service);
                                // Spawn fiber for new session
                                auto nsid = name;
                                auto* nf = sched.spawn([nsid, &sess = *it->second, &stdin_lines, &stdin_eof, &sessions, &sched]() {
                                    sess.mailbox.attach(aura::serve::g_current_fiber);
                                    sess.service.set_wake_eventfd(aura::serve::g_current_fiber->eventfd());
                                    while (sess.active) {
                                        std::string sl;
                                        for (auto sit = stdin_lines.begin(); sit != stdin_lines.end(); ++sit) {
                                            if (sit->find("\"session\":\"" + nsid + "\"") != std::string::npos) {
                                                sl = std::move(*sit);
                                                stdin_lines.erase(sit);
                                                break;
                                            }
                                        }
                                        if (sl.empty()) {
                                            if (stdin_eof) break;
                                            aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
                                            aura::serve::Fiber::yield();
                                            continue;
                                        }
                                        auto c = json_field(sl, "cmd");
                                        if (c == "exec") {
                                            auto code = json_field(sl, "code");
                                            if (!code.empty()) {
                                                auto r = sess.service.exec_with_cache(code);
                                                if (r) {
                                                    std::println("{{\"session\":\"{}\" ,\"status\":\"ok\",\"value\":\"{}\" }}",
                                                                 json_escape(nsid), json_escape(fmt_val(*r, sess.service)));
                                                } else {
                                                    std::println("{{\"session\":\"{}\" ,\"status\":\"error\",\"msg\":\"{}\" }}",
                                                                 json_escape(nsid), json_escape(r.error().format()));
                                                }
                                            }
                                        }
                                    }
                                });
                                it->second->fiber = nf;
                                std::println("{{\"session\":\"{}\" ,\"status\":\"created\",\"name\":\"{}\" }}",
                                             json_escape(sid), json_escape(name));
                            } else {
                                std::println("{{\"session\":\"{}\" ,\"status\":\"error\",\"msg\":\"already exists\"}}",
                                             json_escape(sid));
                            }
                        }
                    } else {
                        std::println("{{\"session\":\"{}\" ,\"status\":\"error\",\"msg\":\"unknown action: {}\" }}",
                                     json_escape(sid), json_escape(action));
                    }

                } else if (cmd == "session-send") {
                    auto target = json_field(line, "target");
                    auto data = json_field(line, "data");
                    if (!target.empty() && !data.empty()) {
                        auto it = sessions.find(target);
                        if (it != sessions.end() && it->second->active) {
                            it->second->mailbox.push(data);
                            std::println("{{\"session\":\"{}\",\"status\":\"sent\",\"target\":\"{}\"}}",
                                         json_escape(sid), json_escape(target));
                        } else {
                            std::println("{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"session not found\"}}",
                                         json_escape(sid));
                        }
                    }

                } else if (cmd == "session-recv") {
                    auto msg = sess.mailbox.pop(true);  // blocking pop (yields)
                    if (!msg.empty()) {
                        std::println("{{\"session\":\"{}\",\"status\":\"msg\",\"data\":\"{}\"}}",
                                     json_escape(sid), json_escape(msg));
                    } else {
                        std::println("{{\"session\":\"{}\",\"status\":\"timeout\"}}",
                                     json_escape(sid));
                    }

                } else {
                    std::println("{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"unknown cmd: {}\"}}",
                                 json_escape(sid), json_escape(cmd));
                }
                std::fflush(stdout);
            }
        });

        sess->fiber = fiber;
    }

    // 7. Run the scheduler
    sched.run();

}

// ── run_serve_async_bench ────────────────────────────

void run_serve_async_bench(const std::string& file_path, int num_workers) {
    // 1. Set stdin to non-blocking
    int flags = ::fcntl(STDIN_FILENO, F_GETFL);
    ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    // 2. Create scheduler with worker threads
    Scheduler sched(num_workers);

    // Register fiber:spawn callback
    aura::messaging::g_fiber_spawn = [&sched](std::function<void()> fn) -> int64_t {
        auto* f = sched.spawn(std::move(fn));
        return static_cast<int64_t>(f ? f->id() : 0);
    };

    // Register fiber:yield callback
    aura::messaging::g_fiber_yield = []() {
        if (aura::serve::g_current_fiber) {
            aura::serve::Fiber::yield();
        }
    };

    // Register fiber blocking callback
    aura::messaging::g_fiber_block = []() {
        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
    };

    // Register async HTTP handler (same as run_serve_async)
    aura::messaging::g_http_post_async = [](const std::string& url,
                                              const std::string& body,
                                              const std::string& auth) -> std::string {
        auto* fiber = aura::serve::g_current_fiber;
        if (!fiber) return {};
        auto evfd = fiber->eventfd();
        if (evfd < 0) return {};

        std::string result;
        std::thread t([evfd, url, body, auth, &result]() {
            int in[2], out[2];
            if (::pipe(in) < 0 || ::pipe(out) < 0) {
                uint64_t v = 1; ::write(evfd, &v, sizeof(v)); return;
            }
            pid_t pid = ::fork();
            if (pid < 0) {
                ::close(in[0]); ::close(in[1]); ::close(out[0]); ::close(out[1]);
                uint64_t v = 1; ::write(evfd, &v, sizeof(v)); return;
            }
            if (pid == 0) {
                ::close(in[1]); ::close(out[0]);
                ::dup2(in[0], STDIN_FILENO); ::dup2(out[1], STDOUT_FILENO);
                ::close(in[0]); ::close(out[1]);
                std::vector<const char*> argv;
                argv.push_back("curl");
                argv.push_back("-s"); argv.push_back("-X"); argv.push_back("POST");
                argv.push_back("--data-binary"); argv.push_back("@-");
                argv.push_back("-H"); argv.push_back("Content-Type: application/json");
                if (!auth.empty()) {
                    auto auth_hdr = "Authorization: Bearer " + auth;
                    argv.push_back("-H");
                    argv.push_back(auth_hdr.c_str());
                }
                argv.push_back("--max-time"); argv.push_back("30");
                argv.push_back("--connect-timeout"); argv.push_back("10");
                argv.push_back(url.c_str());
                argv.push_back(nullptr);
                ::execvp("curl", const_cast<char* const*>(argv.data()));
                ::_exit(1);
            }
            ::close(in[0]); ::close(out[1]);
            ::write(in[1], body.data(), body.size()); ::close(in[1]);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
            std::array<char, 65536> fbuf;
            struct pollfd pfd = {out[0], POLLIN, 0};
            ssize_t nr;
            while (true) {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0) break;
                int pr = ::poll(&pfd, 1, static_cast<int>(remaining));
                if (pr <= 0) break;
                nr = ::read(out[0], fbuf.data(), fbuf.size());
                if (nr <= 0) break;
                result.append(fbuf.data(), static_cast<std::size_t>(nr));
            }
            ::close(out[0]);
            int cstat;
            ::waitpid(pid, &cstat, 0);
            uint64_t v = 1;
            ::write(evfd, &v, sizeof(v));
        });
        t.detach();
        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
        return result;
    };

    // 3. Create default session
    void* shared_workspace_tree = aura::compiler::Evaluator::create_workspace_tree();
    struct BenchSession {
        std::string id;
        aura::compiler::CompilerService service;
    };
    auto sess = std::make_unique<BenchSession>();
    sess->id = "default";
    sess->service.set_session_id("default");
    sess->service.set_workspace_tree(shared_workspace_tree);
    aura::compiler::CompilerService::register_session("default", &sess->service);

    // 4. Read the bench file
    std::ifstream f(file_path);
    if (!f) {
        std::println(std::cerr, "error: cannot open '{}'", file_path);
        return;
    }
    std::string bench_code((std::istreambuf_iterator<char>(f)), {});

    // 5. Spawn a single fiber that runs the bench code
    // The bench code uses fiber:spawn for parallelism; spawned fibers
    // continue running via the scheduler even after this fiber completes.
    sched.spawn([&sess, bench_code = std::move(bench_code), &sched]() {
        // Set wake eventfd for recv/send
        sess->service.set_wake_eventfd(aura::serve::g_current_fiber->eventfd());

        // Evaluate expressions one at a time (same as stdin pipe mode)
        // Split by balanced parentheses to evaluate each expression separately.
        std::string remaining = bench_code;
        bool any_error = false;
        while (!remaining.empty()) {
            // Skip whitespace and comments
            std::size_t start = 0;
            while (start < remaining.size()) {
                auto c = remaining[start];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ++start;
                } else if (c == ';') {
                    // Skip comment to end of line
                    auto nl = remaining.find('\n', start);
                    if (nl == std::string::npos) {
                        remaining.clear();
                        goto done;
                    }
                    start = nl + 1;
                } else {
                    break;
                }
            }
            if (start >= remaining.size()) break;

            // Find the end of this balanced expression
            int depth = 0;
            bool in_str = false;
            std::size_t end = start;
            for (; end < remaining.size(); ++end) {
                auto c = remaining[end];
                if (in_str) {
                    if (c == '\\' && end + 1 < remaining.size()) {
                        ++end;  // skip escaped char
                    } else if (c == '"') {
                        in_str = false;
                    }
                } else if (c == '"') {
                    in_str = true;
                } else if (c == '(' || c == '[') {
                    ++depth;
                } else if ((c == ')' || c == ']') && depth > 0) {
                    if (--depth == 0) {
                        ++end;  // include closing paren
                        break;
                    }
                }
            }
            if (depth != 0) {
                // Unbalanced — evaluate what we have
                end = remaining.size();
            }

            auto expr = remaining.substr(start, end - start);
            remaining.erase(0, end);

            if (!expr.empty()) {
                auto result = sess->service.eval(expr);
                if (!result) {
                    std::print(std::cerr, "eval error on expr (len={}): {}\n  msg: {}\n",
                               expr.size(), expr.substr(0, 120), result.error().format());
                    std::fflush(stderr);
                    any_error = true;
                    break;
                }
            }
        }
        done:
        std::fflush(stdout);
        (void)any_error;
    });

    // 6. Run the scheduler
    sched.run();
}

} // namespace aura::serve
