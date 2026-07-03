# Production Deployment (Issue #677)

Aura ships official container images, HTTP health endpoints, Prometheus metrics, and a documented install layout.

## Container image

```bash
docker build -t aura:local .
docker compose up
curl http://localhost:8080/healthz
curl http://localhost:8080/readyz
curl http://localhost:8080/metrics
```

Image layout:

| Path | Purpose |
|------|---------|
| `/opt/aura/bin/aura` | Runtime binary |
| `/opt/aura/lib/runtime.c` | AOT runtime (via `AURA_RUNTIME_DIR`) |
| `/opt/aura/share/aura/demos` | Example workspaces |

Environment:

- `AURA_RUNTIME_DIR=/opt/aura` — runtime.c lookup (#374)
- `AURA_HTTP_PORT=8080` — enables `/healthz`, `/readyz`, `/metrics`

## HTTP observability endpoints

| Endpoint | Purpose |
|----------|---------|
| `GET /healthz` | Liveness (process up) |
| `GET /readyz` | Readiness (`runtime.c` resolvable + ready flag) |
| `GET /metrics` | Prometheus text (scheduler counters when `--serve-async` active) |

Enable alongside any mode:

```bash
AURA_HTTP_PORT=8080 ./build/aura --serve-async
# or standalone probe server:
./build/aura --health-server 8080
```

## Install layout (`make install`)

```bash
cmake --build build --target aura
cmake --install build --prefix /usr/local
```

Installs:

- `bin/aura`
- `share/aura/runtime.c`
- `share/aura/demos/` (optional)

Set `AURA_RUNTIME_DIR=/usr/local` or rely on FHS fallbacks (`/usr/local/share/aura/runtime.c`).

## Release artifacts

Tagged releases (`v*`) trigger `.github/workflows/release.yml`:

- Reproducible tarball (`scripts/release.sh`)
- CycloneDX SBOM (`dist/aura-sbom.json`)

Local:

```bash
SOURCE_DATE_EPOCH=1704067200 ./scripts/release.sh 1.0.0
```

## Kubernetes probes (example)

```yaml
livenessProbe:
  httpGet:
    path: /healthz
    port: 8080
readinessProbe:
  httpGet:
    path: /readyz
    port: 8080
```

## Upgrade / rollback

1. Pull new image tag or install new tarball.
2. Verify `/readyz` before routing traffic.
3. Roll back by redeploying previous image/tarball; workspace state is session-local in `--serve` / `--serve-async`.