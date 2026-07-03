// http_health.h — Issue #677: /healthz /readyz /metrics HTTP endpoints.

#ifndef AURA_SERVE_HTTP_HEALTH_H
#define AURA_SERVE_HTTP_HEALTH_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace aura::serve::http {

struct HealthConfig {
    std::atomic<bool>* ready = nullptr;
    std::function<std::string()> metrics_prometheus;
    std::function<bool()> runtime_resolved;
};

// Parse AURA_HTTP_PORT (default 0 = disabled). Returns 0 if unset/invalid.
int port_from_env();

// True when runtime.c is resolvable (AURA_RUNTIME_DIR, CWD lib/, install paths).
bool runtime_c_resolved();

// Start background thread serving GET /healthz /readyz /metrics. Idempotent no-op if port<=0.
void start_health_server(int port, const HealthConfig& cfg);

// Request shutdown (joins background thread).
void stop_health_server();

}  // namespace aura::serve::http

#endif  // AURA_SERVE_HTTP_HEALTH_H