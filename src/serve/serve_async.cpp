// serve/serve_async.cpp — Async serve mode implementation
#include "serve_async.h"
#include "messaging_bridge.h"
#include "scheduler.h"
#include "fiber.h"
#include "mailbox.h"

#include <print>
#include <fcntl.h>
#include <unistd.h>
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

    std::unordered_map<std::string, std::unique_ptr<Session>> sessions;
    std::string active_session = "default";
    {
        auto& sess = sessions["default"];
        sess = std::make_unique<Session>();
        sess->id = "default";
        sess->service.set_session_id("default");
        aura::compiler::CompilerService::register_session("default", &sess->service);
    }

    // 6. Spawn session fibers (one per session)
    // For now, spawn one fiber for the default session
    // In the future, spawn as needed
    for (auto& [sid, sess] : sessions) {
        auto* fiber = sched.spawn([sid = sid, &sess = *sess, &stdin_lines, &stdin_eof, &sessions]() {
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
                            for_me = (it->find("\"session\"") == std::string::npos);
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
                    // Execute synchronously (will yield on recv when Phase 3 is implemented)
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
