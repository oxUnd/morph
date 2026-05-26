# morph-fastcgi

A FastCGI front-end for the Morph terminal-native agent — letting you put
nginx (or any FastCGI-aware server) in front of Morph and serve a Web UI
without bringing an HTTP stack into the core binary.

> The CLI process and the FastCGI worker share the same SQLite database
> in WAL mode, so an agent can be driven from either surface
> simultaneously.

---

## Layout

```
src/fastcgi/
├── CMakeLists.txt
├── PATCHES.md          # optional agent-side hooks (additive)
├── README.md
├── main.c              # FCGX_Init + worker pool + signal handling
├── router.{c,h}        # /pattern/:id dispatcher (no regex)
├── auth.{c,h}          # Bearer + X-Remote-User
├── fcgi_io.{c,h}       # request_t, JSON & SSE helpers
├── session_store.{c,h} # SQLite WAL multi-session backend
├── event_sink.{c,h}    # convenience wrappers around events_publish
├── action_pump.{c,h}   # drain-with-wait for Web → Agent commands
└── handlers/
    ├── handlers.h
    ├── health.c        # GET  /api/health
    ├── sessions.c      # CRUD /api/sessions
    ├── turns.c         # POST /api/sessions/:id/turns
    ├── canvas.c        # GET/POST/PATCH /api/sessions/:id/canvas/...
    ├── actions.c       # POST /api/sessions/:id/actions
    └── events.c        # GET  /api/sessions/:id/events  (SSE)
```

---

## Build

```bash
sudo apt install libfcgi-dev libsqlite3-dev libcjson-dev libcurl4-openssl-dev

cd morph
cmake -S . -B build -DBUILD_FASTCGI=ON
cmake --build build -j
```

The binary lands at `build/src/fastcgi/morph-fastcgi`.

---

## Run

```bash
export MORPH_FCGI_LISTEN="unix:/run/morph-fastcgi.sock"
export MORPH_FCGI_DB="/var/lib/morph/morph.db"
export MORPH_FCGI_WORKERS=8
# At least one authentication mode is required (both => trust-header wins):
export MORPH_FCGI_SECRET="my-bearer-secret"
# Use only behind a trusted proxy that removes client-supplied identity headers:
export MORPH_FCGI_TRUST_HDR="X-Remote-User"

./build/src/fastcgi/morph-fastcgi
```

`MORPH_FCGI_LISTEN` accepts `unix:/path/sock`, `:9000`, or `127.0.0.1:9000`.
The process refuses to start unless `MORPH_FCGI_SECRET` or
`MORPH_FCGI_TRUST_HDR` is set. When trust-header authentication is used, the
configured header is the only accepted identity source.

---

## API surface

| Method | Path                                          | Notes                       |
| ------ | --------------------------------------------- | --------------------------- |
| GET    | /api/health                                   | liveness                    |
| POST   | /api/sessions                                 | `{name, model}` → `{id}`    |
| GET    | /api/sessions                                 | sessions owned by user      |
| GET    | /api/sessions/:id                             |                             |
| DELETE | /api/sessions/:id                             | MVP: ack only               |
| POST   | /api/sessions/:id/turns                       | `{prompt}` → 202 + SSE      |
| GET    | /api/sessions/:id/events                      | SSE, honours Last-Event-ID  |
| POST   | /api/sessions/:id/actions                     | `{type, payload}`           |
| GET    | /api/sessions/:id/canvas                      |                             |
| POST   | /api/sessions/:id/canvas/nodes                |                             |
| PATCH  | /api/sessions/:id/canvas/nodes/:node          |                             |

SSE event types: `ready`, `turn_start`, `thought`, `tool_call`,
`tool_result`, `reflection`, `final`, `turn_end`, `canvas_node_added`,
`canvas_node_patched`, `error`.

---

## Deployment

Examples in `deploy/`:

- `nginx.conf`         — fastcgi_pass + a separate location block for SSE
                          with `fastcgi_buffering off`, `fastcgi_read_timeout 1h`
- `morph-fastcgi.service` — hardened systemd unit
- `Dockerfile`         — multi-stage debian:bookworm-slim build

---

## Multi-process consistency

Both `morph` (CLI) and `morph-fastcgi` open the same SQLite DB with WAL +
`busy_timeout=5000`.  Writers serialise; readers don't block.  Within a
single FastCGI process the pthread cond-var is broadcast on every event
write so all SSE workers wake up immediately.  Across processes, the
worst-case latency is one `events_wait_after` poll (default 15 s, also the
heartbeat cadence).

---

## Optional agent integration

See `PATCHES.md` for two small additive patches that enable cancellation
and per-session context factory.  The base build runs without them and
returns a clear `error` event so the missing hook is observable.
