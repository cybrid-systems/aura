# Issue #677: official Aura production image (multi-stage).
# Build: docker build -t aura:local .
# Run:   docker run --rm -p 8080:8080 -e AURA_HTTP_PORT=8080 aura:local

FROM ghcr.io/cybrid-systems/aura-ci:v1.0.1 AS builder
WORKDIR /src
COPY . .
ENV CCACHE_DISABLE=1
RUN python3 build.py repro build

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/aura
COPY --from=builder /src/build_repro/aura /opt/aura/bin/aura
COPY --from=builder /src/lib/runtime.c /opt/aura/lib/runtime.c
COPY --from=builder /src/demos /opt/aura/share/aura/demos
ENV PATH=/opt/aura/bin:$PATH
ENV AURA_RUNTIME_DIR=/opt/aura
ENV AURA_HTTP_PORT=8080
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1
CMD ["aura", "--health-server", "8080"]