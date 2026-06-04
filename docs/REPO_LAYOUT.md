# Repository layout

```text
pinback/
├── src/                    Hand-written C99 server sources (+ static_assets.h)
├── build/                  Out-of-tree server outputs (gitignored)
│   ├── pinback-server
│   ├── fake-ds4-agent
│   ├── run_tests
│   └── generated/static_assets.c
├── ui/
│   ├── app/                Shipped cockpit UI (embedded into the server)
│   └── shiki-bundle/       npm/esbuild toolchain to refresh ui/app/vendor/shiki/
├── tests/                  C unit + integration tests
│   └── support/            fake-ds4-agent test double (source)
├── scripts/
│   ├── embed/              gen-static-assets.sh
│   └── qa/                 pinback-smoke, pinback-e2e
├── platform/               Native webview shells + shared icons (platform/icons/)
├── ingress/                Caddy, Tailscale, WireGuard examples
├── runtime/                DS4 runtime supervisor + web PTY prototype (optional)
├── experiments/            Transport probes and research scripts (not shipped)
├── docs/
│   ├── REPO_LAYOUT.md      this file
│   ├── architecture/       Normative design + transport findings
│   ├── operations/         Runtime cockpit + deployment notes
│   ├── postmortems/
│   ├── backlog/
│   └── assets/marketing/   Screenshots and release imagery (not runtime code)
├── dist/                   Local release drops (gitignored)
├── Makefile
├── CONTRIBUTING.md
└── LICENSE
```

## UI pipeline

```text
ui/shiki-bundle/  ──►  ui/app/vendor/shiki/*  ──►  make embed  ──►  build/generated/static_assets.c  ──►  build/pinback-server
```

## Build outputs (never commit)

| Path | Contents |
|------|----------|
| `build/` | `pinback-server`, `fake-ds4-agent`, `run_tests`, generated embed |
| `dist/` | Local packaged platform artifacts (APK, zip, tar.gz from CI or manual drops) |
| `platform/*/build/` | Per-platform shell compile trees |
| `runtime/ui/dist/` | Vite build for the runtime prototype UI (`cd runtime/ui && bun run build`) |

## What is not the product

| Path | Purpose |
|------|---------|
| `experiments/transport-probe/` | Historical DS4 transport archaeology |
| `runtime/` | Owned DS4 runtime cockpit prototype |
| `docs/assets/marketing/` | Marketing / screenshot assets |

## Legacy paths (pre-2026-06 reorg)

| Old path | New path |
|----------|----------|
| `web/` | `ui/app/` |
| `web-build/` | `ui/shiki-bundle/` |
| `tools/gen-static-assets.sh` | `scripts/embed/gen-static-assets.sh` |
| `tools/pinback-smoke` | `scripts/qa/pinback-smoke` |
| `tools/pinback-e2e` | `scripts/qa/pinback-e2e` |
| `tools/fake-ds4-agent.c` | `tests/support/fake-ds4-agent.c` → `build/fake-ds4-agent` |
| `tools/ds4_*` | `runtime/` |
| `tools/transport-probe/` | `experiments/transport-probe/` |
| `src/static_assets.c` | `build/generated/static_assets.c` |
| `pinback-server` (repo root) | `build/pinback-server` |
| `release-artifacts/` | `dist/` |
