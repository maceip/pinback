# Pinback Long-Markdown Fidelity Fixture

This is a deliberately tricky multi-section markdown reply meant to stress the markdown TextMate grammar's `\G` continuation logic. The previous architecture had token-boundary drift on long markdown; this fixture is the regression gate.

## Sections

- markdown structure with nested lists
- fenced code blocks for every language we ship a grammar for
- inline `code` and **emphasis** mixed with fences
- a table
- a hard case: fence followed immediately by a list

### Sample 1: `rust`

Some prose before the fence to make sure paragraph -> fence transition works. We mention `rust` inline and continue.

```rust
fn fibonacci(n: u64) -> u64 {
    let mut a: u64 = 0;
    let mut b: u64 = 1;
    for _ in 0..n {
        let t = a + b;
        a = b;
        b = t;
    }
    a
}

#[derive(Debug, Clone)]
pub struct Outfox<'a, K: Hash> {
    where_: &'a [K],
    seal: Option<Box<dyn Fn(&K) -> Result<(), Error> + Send + Sync>>,
}
```

Continuation paragraph immediately after the fence.

1. List item with inline `var x = 0`
2. Second item — _italic_ and **bold** mixed.
3. Third item with a [link](https://example.com).

### Sample 2: `python`

Some prose before the fence to make sure paragraph -> fence transition works. We mention `python` inline and continue.

```python
import asyncio
from typing import AsyncIterator

async def stream_tokens(prompt: str) -> AsyncIterator[str]:
    async with httpx.AsyncClient() as cx:
        r = await cx.post("/v1/chat/completions",
                          json={"messages": [{"role":"user","content": prompt}],
                                "stream": True})
        async for line in r.aiter_lines():
            if line.startswith("data: "):
                yield line[6:]
```

Continuation paragraph immediately after the fence.

1. List item with inline `var x = 1`
2. Second item — _italic_ and **bold** mixed.
3. Third item with a [link](https://example.com).

### Sample 3: `c`

Some prose before the fence to make sure paragraph -> fence transition works. We mention `c` inline and continue.

```c
static int pin_event_log_append(pin_event_log *log,
                                const char *kind,
                                const char *payload, size_t plen) {
    if (!log || !kind) return -1;
    pthread_rwlock_wrlock(&log->rw);
    long long seq = ++log->newest_seq;
    pthread_rwlock_unlock(&log->rw);
    return (int)seq;
}
```

Continuation paragraph immediately after the fence.

1. List item with inline `var x = 2`
2. Second item — _italic_ and **bold** mixed.
3. Third item with a [link](https://example.com).

### Sample 4: `javascript`

Some prose before the fence to make sure paragraph -> fence transition works. We mention `javascript` inline and continue.

```javascript
const es = new EventSource("/api/events?after=0");
es.addEventListener("event", (ev) => {
  const env = JSON.parse(ev.data);
  if (env.kind === "answer") {
    document.getElementById("out").textContent += env.payload.text;
  }
});
```

Continuation paragraph immediately after the fence.

1. List item with inline `var x = 3`
2. Second item — _italic_ and **bold** mixed.
3. Third item with a [link](https://example.com).

### Sample 5: `json`

Some prose before the fence to make sure paragraph -> fence transition works. We mention `json` inline and continue.

```json
{
  "version": "pinback-server/0.1",
  "upstream": { "reachable": true, "state": "ready", "model": "ds4" },
  "events":   { "generation": 4, "oldest_seq": 1, "newest_seq": 7142 }
}
```

Continuation paragraph immediately after the fence.

1. List item with inline `var x = 4`
2. Second item — _italic_ and **bold** mixed.
3. Third item with a [link](https://example.com).

### Sample 6: `go`

Some prose before the fence to make sure paragraph -> fence transition works. We mention `go` inline and continue.

```go
func (h *Hub) Broadcast(ev Event) {
    h.mu.RLock()
    defer h.mu.RUnlock()
    for sub := range h.subs {
        select {
        case sub.ch <- ev:
        default:
            log.Printf("drop subscriber %s", sub.id)
        }
    }
}
```

Continuation paragraph immediately after the fence.

1. List item with inline `var x = 5`
2. Second item — _italic_ and **bold** mixed.
3. Third item with a [link](https://example.com).

### Sample 7: `yaml`

Some prose before the fence to make sure paragraph -> fence transition works. We mention `yaml` inline and continue.

```yaml
service:
  name: pinback-server
  bind: 127.0.0.1:8089
  upstream:
    host: 127.0.0.1
    port: 8000
    cors: true
  log:
    path: /Users/mac/.pinback/events.log
    ring_capacity: 4096
```

Continuation paragraph immediately after the fence.

1. List item with inline `var x = 6`
2. Second item — _italic_ and **bold** mixed.
3. Third item with a [link](https://example.com).

### Sample 8: `bash`

Some prose before the fence to make sure paragraph -> fence transition works. We mention `bash` inline and continue.

```bash
#!/usr/bin/env bash
set -euo pipefail
URL=${1:-http://127.0.0.1:8089}
curl -sS -H 'Content-Type: application/json' \
     -X POST -d '{"text":"hi"}' \
     "$URL/api/input" \
  | jq -r '.ok'
```

Continuation paragraph immediately after the fence.

1. List item with inline `var x = 7`
2. Second item — _italic_ and **bold** mixed.
3. Third item with a [link](https://example.com).

### Sample 9: `typescript`

Some prose before the fence to make sure paragraph -> fence transition works. We mention `typescript` inline and continue.

```typescript
import { createHighlighterCore } from "shiki/core";
import { createOnigurumaEngine } from "shiki/engine/oniguruma";
import md from "@shikijs/langs/markdown";

const hl = await createHighlighterCore({
  engine: await createOnigurumaEngine(import("shiki/wasm")),
  themes: ["vitesse-dark"],
  langs: [md],
});
```

Continuation paragraph immediately after the fence.

1. List item with inline `var x = 8`
2. Second item — _italic_ and **bold** mixed.
3. Third item with a [link](https://example.com).

### Sample 10: `html`

Some prose before the fence to make sure paragraph -> fence transition works. We mention `html` inline and continue.

```html
<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>pinback</title></head>
<body>
  <script src="/app.js" defer></script>
</body>
</html>
```

Continuation paragraph immediately after the fence.

1. List item with inline `var x = 9`
2. Second item — _italic_ and **bold** mixed.
3. Third item with a [link](https://example.com).

## Edge cases

Fence with no language tag:

```
plain block — no language declared
should render as txt
```

Fence with leading whitespace inside body:

```python
    indented = True
    if indented:
        pass
```

Inline backticks: `single`, ``with `inner` backtick``, ```triple inline```.

## Table

| key | type | description |
|-----|------|-------------|
| `seq` | int64 | monotonic sequence id |
| `generation` | int64 | bumped on /api/control clear |
| `kind` | string | one of user/answer/answer_end/error/aborted |

## Closing prose

This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. This file is intentionally over 10 KB. 
