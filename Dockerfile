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
    && rm -rf /var/lib/apt/lists/* \
    # Issue #675 follow-up: Trivy DS-0002 (HIGH) requires
    # a non-root USER directive. Create a dedicated
    # `aura` user + group; the runtime image runs as aura
    # (not root) so the aura server process can't escape
    # the container via a root-owned shell. nologin shell
    # prevents interactive login (the aura process is the
    # only intended entrypoint).
    && groupadd -r aura && useradd -r -g aura -d /opt/aura -s /usr/sbin/nologin aura \
    && mkdir -p /opt/aura/bin /opt/aura/lib /opt/aura/share/aura \
    && chown -R aura:aura /opt/aura
WORKDIR /opt/aura
COPY --from=builder --chown=aura:aura /src/build_repro/aura /opt/aura/bin/aura
COPY --from=builder --chown=aura:aura /src/lib/runtime.c /opt/aura/lib/runtime.c
ENV PATH=/opt/aura/bin:$PATH
ENV AURA_RUNTIME_DIR=/opt/aura
ENV AURA_HTTP_PORT=8080
EXPOSE 8080
# HEALTHCHECK uses 127.0.0.1 so the container itself can
# reach the health server without network privileges that
# would normally need root. aura runs the server; the
# HEALTHCHECK curls as a separate process under the same
# aura user.
# /readyz matches docker-compose.yml healthcheck + is the
# Kubernetes-conventional readiness endpoint. 5s timeout
# matches compose.
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/readyz || exit 1
USER aura
CMD ["aura", "--health-server", "8080"]
