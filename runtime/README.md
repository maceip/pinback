# DS4 runtime cockpit (optional)

Separate subsystem for owned DS4 runtime profiles, web PTY bridge, and prototype UI.

| Path | Role |
|------|------|
| `ds4_runtime_supervisor.py` | Profile supervisor |
| `ds4_agent_webpty.py` | Authenticated web PTY bridge |
| `ui/` | Vite prototype UI |
| `assets/` | Design mock JPEGs served by the bridge |
| `ds4_bucket0_live_smoke.sh` | Bucket 0 live acceptance |
| `test_ds4_webpty_supervisor.py` | Supervisor regression tests |

Not embedded in `build/pinback-server`. See `docs/operations/ds4-runtime-cockpit.md`.
