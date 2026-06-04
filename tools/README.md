# tools/ (removed)

This directory was reorganized. Use:

| Old path | New path |
|----------|----------|
| `tools/gen-static-assets.sh` | `scripts/embed/gen-static-assets.sh` |
| `tools/pinback-smoke` | `scripts/qa/pinback-smoke` |
| `tools/pinback-e2e` | `scripts/qa/pinback-e2e` |
| `tools/fake-ds4-agent.c` | `tests/support/fake-ds4-agent.c` → `build/fake-ds4-agent` |
| `tools/ds4_*` | `runtime/` |
| `tools/transport-probe/` | `experiments/transport-probe/` |

See [docs/REPO_LAYOUT.md](../docs/REPO_LAYOUT.md).
