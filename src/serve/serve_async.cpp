// serve/serve_async.cpp — Async serve mode implementation
#include "serve_async.h"
#include "messaging_bridge.h"
#include "scheduler.h"
#include "fiber.h"
#include "mailbox.h"

#include <print>
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
#include <deque>
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
    auto key = "\"" + field + "\":\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return {};
    pos += key.size();
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            val += json[pos + 1];
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

void run_serve_async() {
    // 1. Set stdin to non-blocking
    int flags = ::fcntl(STDIN_FILENO, F_GETFL);
    ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    // Register fiber blocking callback for CompilerService::pop_message
    aura::messaging::g_fiber_block = []() {
        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
    };

    // 2. Create scheduler
    Scheduler sched;

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
                stdin_lines.push_back(buf.substr(0, nl));
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

    // Register async HTTP handler (uses thread + fork+exec curl CLI for non-blocking HTTP)
    // Thread handles the blocking fork+exec while the fiber yields.
    // When the thread completes, it signals the fiber via eventfd.
    aura::messaging::g_http_post_async = [](const std::string& url,
                                              const std::string& body,
                                              const std::string& auth) -> std::string {
        auto* fiber = aura::serve::g_current_fiber;
        if (!fiber) return {};
        auto evfd = fiber->eventfd();
        if (evfd < 0) return {};

        std::string result;
        std::thread t([evfd, url, body, auth, &result]() {
            // Create pipes for child communication
            int in[2], out[2];
            if (::pipe(in) < 0 || ::pipe(out) < 0) {
                uint64_t v = 1; ::write(evfd, &v, sizeof(v));
                return;
            }

            pid_t pid = ::fork();
            if (pid < 0) {
                ::close(in[0]); ::close(in[1]); ::close(out[0]); ::close(out[1]);
                uint64_t v = 1; ::write(evfd, &v, sizeof(v));
                return;
            }

            if (pid == 0) {
                // Child: exec curl
                ::close(in[1]); ::close(out[0]);
                ::dup2(in[0], STDIN_FILENO); ::dup2(out[1], STDOUT_FILENO);
                ::close(in[0]); ::close(out[1]);

                std::vector<const char*> argv;
                argv.push_back("curl");
                argv.push_back("-s");
                argv.push_back("-X"); argv.push_back("POST");
                argv.push_back("--data-binary"); argv.push_back("@-");
                argv.push_back("-H"); argv.push_back("Content-Type: application/json");
                if (!auth.empty()) {
                    auto* hdr = new std::string("Authorization: Bearer " + auth);
                    argv.push_back("-H");
                    argv.push_back(hdr->c_str());
                }
                argv.push_back("--max-time"); argv.push_back("30");
                argv.push_back("--connect-timeout"); argv.push_back("10");
                argv.push_back(url.c_str());
                argv.push_back(nullptr);

                ::execvp("curl", const_cast<char* const*>(argv.data()));
                ::_exit(1);
            }

            // Parent thread: send body, read response
            ::close(in[0]); ::close(out[1]);
            ::write(in[1], body.data(), body.size()); ::close(in[1]);

            std::array<char, 4096> fbuf;
            ssize_t nr;
            while ((nr = ::read(out[0], fbuf.data(), fbuf.size())) > 0)
                result.append(fbuf.data(), static_cast<std::size_t>(nr));
            ::close(out[0]);

            int cstat;
            ::waitpid(pid, &cstat, 0);

            // Signal the fiber that HTTP is done
            uint64_t v = 1;
            ::write(evfd, &v, sizeof(v));
        });
        t.detach();

        // Yield — scheduler will resume this fiber when thread signals eventfd
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
                            for_me = (it->find("\"session\":\"") == std::string::npos);
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

} // namespace aura::serve
