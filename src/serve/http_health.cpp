// http_health.cpp — Issue #677 minimal HTTP health/metrics server.

#include "http_health.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <thread>

namespace aura::serve::http {
namespace {

std::thread g_server_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_default_ready{true};
HealthConfig g_cfg{};

std::string http_response(int code, const char* status, const std::string& body,
                          const char* content_type) {
    return "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n"
           + "Content-Type: " + content_type + "\r\n"
           + "Content-Length: " + std::to_string(body.size()) + "\r\n"
           + "Connection: close\r\n\r\n" + body;
}

void handle_client(int fd) {
    char buf[2048];
    const auto n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        ::close(fd);
        return;
    }
    buf[n] = '\0';
    std::string req(buf);
    const auto line_end = req.find("\r\n");
    const std::string line = line_end == std::string::npos ? req : req.substr(0, line_end);
    const auto sp1 = line.find(' ');
    const auto sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    std::string path = "/";
    if (sp1 != std::string::npos && sp2 != std::string::npos && sp2 > sp1 + 1)
        path = line.substr(sp1 + 1, sp2 - sp1 - 1);

    std::string resp;
    if (path == "/healthz") {
        resp = http_response(200, "OK", "ok\n", "text/plain");
    } else if (path == "/readyz") {
        const bool ready_flag = g_cfg.ready ? g_cfg.ready->load(std::memory_order_acquire)
                                            : g_default_ready.load(std::memory_order_acquire);
        const bool runtime_ok = g_cfg.runtime_resolved ? g_cfg.runtime_resolved() : true;
        if (ready_flag && runtime_ok)
            resp = http_response(200, "OK", "ready\n", "text/plain");
        else
            resp = http_response(503, "Service Unavailable", "not ready\n", "text/plain");
    } else if (path == "/metrics") {
        std::string body = "# HELP aura_up Aura process is running.\n# TYPE aura_up gauge\naura_up 1\n";
        if (g_cfg.metrics_prometheus)
            body += g_cfg.metrics_prometheus();
        resp = http_response(200, "OK", body, "text/plain; version=0.0.4");
    } else {
        resp = http_response(404, "Not Found", "not found\n", "text/plain");
    }
    (void)::send(fd, resp.data(), resp.size(), 0);
    ::close(fd);
}

void server_loop(int port) {
    const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0)
        return;
    int opt = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(srv);
        return;
    }
    if (::listen(srv, 16) != 0) {
        ::close(srv);
        return;
    }
    while (!g_stop.load(std::memory_order_acquire)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(srv, &fds);
        timeval tv{.tv_sec = 1, .tv_usec = 0};
        const int r = ::select(srv + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0)
            continue;
        const int client = ::accept(srv, nullptr, nullptr);
        if (client >= 0)
            handle_client(client);
    }
    ::close(srv);
}

}  // namespace

bool runtime_c_resolved() {
    auto readable = [](const std::string& path) {
        return ::access(path.c_str(), R_OK) == 0;
    };
    if (const char* dir = std::getenv("AURA_RUNTIME_DIR")) {
        const std::string base(dir);
        if (readable(base + "/runtime.c") || readable(base + "/lib/runtime.c"))
            return true;
    }
    for (const char* rel : {"lib/runtime.c", "../lib/runtime.c",
                             "/usr/local/share/aura/runtime.c",
                             "/usr/share/aura/runtime.c",
                             "/opt/aura/share/runtime.c"}) {
        if (readable(rel))
            return true;
    }
    return false;
}

int port_from_env() {
    if (const char* raw = std::getenv("AURA_HTTP_PORT")) {
        char* end = nullptr;
        const long v = std::strtol(raw, &end, 10);
        if (end != raw && *end == '\0' && v > 0 && v < 65536)
            return static_cast<int>(v);
    }
    return 0;
}

void start_health_server(int port, const HealthConfig& cfg) {
    if (port <= 0 || g_server_thread.joinable())
        return;
    g_cfg = cfg;
    g_stop.store(false, std::memory_order_release);
    g_server_thread = std::thread([port] { server_loop(port); });
}

void stop_health_server() {
    g_stop.store(true, std::memory_order_release);
    if (g_server_thread.joinable())
        g_server_thread.join();
}

}  // namespace aura::serve::http