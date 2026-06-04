# Contributing to pinback

## Build

```sh
make                 # build/pinback-server + build/fake-ds4-agent
make test            # unit + integration tests
make embed           # regenerate build/generated/static_assets.c from ui/app/
```

Fresh clones need `make` (or `make embed`) once so `build/generated/static_assets.c`
exists before linking.

## Dev server

```sh
make
./build/pinback-server --dev --agent-bin ./build/fake-ds4-agent --workspace "$(pwd)"
```

`--dev` reads the cockpit from `ui/app/` on every request instead of the embedded bundle.

## Smoke tests

```sh
make smoke URL=http://127.0.0.1:8088
platform/smoke-test.sh
```

## UI changes

1. Edit files under `ui/app/`.
2. If Shiki vendor output changed, rebuild from `ui/shiki-bundle/` (see `ui/shiki-bundle/README.md`).
3. Run `make embed && make`.

## Layout

See [docs/REPO_LAYOUT.md](docs/REPO_LAYOUT.md).

## Pitfalls

- Do not run `make clean` inside Orb/Linux VMs against a shared Mac repo mount — it can corrupt host object files.
- `experiments/` and `runtime/` are not part of the shipping `pinback-server` binary unless explicitly integrated.
- Never commit `build/` outputs or `*.o` files.
