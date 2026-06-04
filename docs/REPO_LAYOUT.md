# Repository layout

```text
pinback/
├── src/                    Hand-written C99 server sources (+ static_assets.h)
├── build/                  Out-of-tree outputs (gitignored)
│   ├── pinback-server
│   ├── fake-ds4-agent
│   └── generated/static_assets.c
├── ui/
│   ├── app/                Shipped cockpit UI (embedded into the server)
│   └── shiki-bundle/       npm/esbuild toolchain to refresh ui/app/vendor/shiki/
├── tests/                  C unit + integration tests
│   └── support/            fake-ds4-agent test double
├── scripts/
│   ├── embed/              gen-static-assets.sh
│   └── qa/                 pinback-smoke, pinback-e2e
├── platform/               Native webview shells (macOS, iOS, Linux, Windows, Android)
├── ingress/                Caddy, Tailscale, WireGuard examples
├── runtime/                DS4 runtime supervisor + web PTY prototype (optional subsystem)
├── experiments/            Transport probes and research scripts (not shipped)
├── docs/
│   ├── architecture/       Normative design + transport findings
│   ├── operations/         Runtime cockpit + deployment notes
│   ├── postmortems/
│   └── backlog/
├── dist/                   Local release drops (gitignored)
└── Makefile
```

## UI pipeline

```text
ui/shiki-bundle/  ──►  ui/app/vendor/shiki/*  ──►  make embed  ──►  build/generated/static_assets.c  ──►  build/pinback-server
```

## What is not the product

| Path | Purpose |
|------|---------|
| `experiments/transport-probe/` | Historical DS4 transport archaeology |
| `runtime/` | Owned DS4 runtime cockpit prototype |
| `dist/` | Local packaged platform artifacts |
