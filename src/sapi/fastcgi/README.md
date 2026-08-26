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
src/sapi/fastcgi/
├── CMakeLists.txt
├── PATCHES.md          # retired weak-symbol bridge notes
├── README.md
├── main.c              # FCGX_Init + worker pool + signal handling
├── router.{c,h}        # /pattern/:id dispatcher (no regex)
├── auth.{c,h}          # session token + Basic Auth + X-Remote-User
├── security.{c,h}      # PBKDF2-SHA256, random ID, constant-time compare
├── fcgi_io.{c,h}       # request_t, JSON & SSE helpers
├── session_store.{c,h} # SQLite WAL multi-session backend + login tokens
├── event_sink.{c,h}    # convenience wrappers around events_publish
├── action_pump.{c,h}   # drain-with-wait for Web → Agent commands
├── agent_bridge.c      # shared runtime lifecycle integration
└── handlers/
    ├── handlers.h
    ├── health.c        # GET  /api/health
    ├── users.c         # POST /api/install, /api/signup, /api/login, /api/logout
    ├── sessions.c      # CRUD /api/sessions
    ├── turns.c         # POST /api/sessions/:id/turns
    ├── canvas.c        # GET/POST/PATCH /api/sessions/:id/canvas/...
    ├── actions.c       # POST /api/sessions/:id/actions
    ├── events.c        # GET  /api/sessions/:id/events  (SSE)
    └── artifacts.c     # GET  /api/sessions/:id/artifacts, /api/artifacts/:id
```

---

## Authentication

morph-fastcgi supports three authentication modes, tried in order:

### 1. Trusted proxy header (highest priority)

Set `MORPH_FCGI_TRUST_HDR` to the HTTP header name your reverse proxy
injects (e.g. `X-Remote-User`).  The header value becomes the identity.

> Only use behind a trusted proxy that strips client-supplied identity
> headers.  A misconfigured proxy allows identity spoofing.

### 2. Session token (recommended for web UI)

Login via `POST /api/login` with HTTP Basic Auth → receive a random
Bearer token → use `Authorization: Bearer <token>` for all subsequent
requests.

Tokens are stored as individual files under `/tmp/morph-sess/` (like PHP
sessions) with a 24-hour TTL.  An in-memory hash cache avoids file I/O
on every request; the cache is kept in sync on login/logout.

Flow:
```
1. POST /api/login  (Basic Auth) → {token, user_id, username, role, expires_at}
2. All requests: Authorization: Bearer <token>
3. POST /api/logout → token revoked (file deleted + cache cleared)
```

### 3. HTTP Basic Auth (fallback)

If no trust-header or Bearer token matches, Basic Auth is tried against
the `fcgi_users` table with PBKDF2-SHA256 password verification.

This is used internally by `POST /api/login` and during initial setup
(`POST /api/install`).

### First-run setup

When no users exist (`GET /api/health` returns `setup_required: true`),
only `POST /api/install` and `GET /api/health` are accessible.  The
install endpoint creates the initial admin user.

---

## Build

```bash
sudo apt install libfcgi-dev libsqlite3-dev libcjson-dev libcurl4-openssl-dev

cd morph
cmake -S . -B build -DBUILD_FASTCGI=ON
cmake --build build -j
```

The binary lands at `build/bin/morph-fastcgi`.

---

## Run

```bash
export MORPH_FCGI_LISTEN="unix:/run/morph-fastcgi.sock"
export MORPH_FCGI_DB="/var/lib/morph/morph.db"
export MORPH_FCGI_WORKERS=128
export MORPH_FCGI_RUNTIME_MIN_WORKERS=8
export MORPH_FCGI_RUNTIME_MAX_WORKERS=64
# Optional: trust a proxy-injected identity header
export MORPH_FCGI_TRUST_HDR="X-Remote-User"
# Optional: override artifact output directory
export MORPH_FCGI_OUTPUT_DIR="/var/lib/morph/output"

./build/bin/morph-fastcgi
```

`MORPH_FCGI_LISTEN` accepts `unix:/path/sock`, `:9000`, or `127.0.0.1:9000`.

---

## API surface

| Method | Path                                          | Notes                          |
| ------ | --------------------------------------------- | ------------------------------ |
| GET    | /api/health                                   | liveness, setup and runtime-pool status |
| POST   | /api/install                                  | create admin (setup only)      |
| POST   | /api/signup                                   | create user (if enabled)       |
| POST   | /api/login                                    | Basic Auth → Bearer token      |
| POST   | /api/logout                                   | revoke current token           |
| GET    | /api/me/quota                                 | current user quota             |
| POST   | /api/sessions                                 | `{name, model}` → `{id}`       |
| GET    | /api/sessions                                 | sessions owned by user         |
| GET    | /api/sessions/:id                             |                                |
| DELETE | /api/sessions/:id                             | 409 if turn in progress        |
| POST   | /api/sessions/:id/turns                       | `{prompt}` → 202 + SSE         |
| GET    | /api/sessions/:id/events                      | SSE, honours Last-Event-ID     |
| POST   | /api/sessions/:id/actions                     | `{type, payload}`              |
| GET    | /api/sessions/:id/canvas                      |                                |
| POST   | /api/sessions/:id/canvas/nodes                |                                |
| PATCH  | /api/sessions/:id/canvas/nodes/:node          |                                |
| GET    | /api/sessions/:id/artifacts                   | list artifacts for session     |
| GET    | /api/artifacts/:artifact/meta                 | artifact metadata              |
| GET    | /api/artifacts/:artifact                      | download artifact binary       |

SSE event types: `ready`, `turn_start`, `thought`, `tool_call`,
`tool_stream`, `tool_result`, `reflection`, `final`, `turn_end`, `artifact_ready`,
`canvas_node_added`, `canvas_node_patched`, `ask_user`, `approval_required`,
`operation_approval_required`, `error`.

### Agent capabilities

FastCGI uses the same runtime bootstrap as the CLI. At process startup it
discovers Skills, loads extensions that support the `fastcgi` front, registers
scheduled tasks and sub-agent tools, and connects configured MCP servers with
`auto_connect = true`. MCP tools, resources, and prompts are registered in the
shared tool registry before requests are accepted.

Interactive turns use `POST /api/sessions/:id/actions`:

- `{"type":"prompt","payload":{"text":"..."}}` steers a turn or answers
  a free-form `ask_user` request;
- `{"type":"answer","payload":{"answers":["..."]}}` answers a structured
  `ask_user` request;
- `{"type":"approve","payload":{}}` approves a pending HITL or filesystem
  operation;
- `{"type":"approve","payload":{"always":true}}` persists the grant where
  supported;
- `{"type":"reject","payload":{}}` denies it;
- `{"type":"cancel","payload":{}}` cancels at the next ReAct checkpoint.

Turn execution is driven by the shared structured event system documented in
`docs/event-system.md`. FastCGI maps `react.*`, `tool.*`, and `artifact.ready`
events to the SSE names above for compatibility with existing GUI clients.

---

## Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `MORPH_FCGI_LISTEN` | `unix:/run/morph-fastcgi.sock` | Listen spec: `unix:/path`, `:port`, `host:port` |
| `MORPH_FCGI_DB` | `/var/lib/morph/morph.db` | Path to SQLite database |
| `MORPH_FCGI_WORKERS` | `128` | FastCGI/SSE connection workers (1–512) |
| `MORPH_FCGI_RUNTIME_MIN_WORKERS` | `8` | Prewarmed agent runtimes (1–256) |
| `MORPH_FCGI_RUNTIME_MAX_WORKERS` | `64` | Maximum elastic agent runtimes (1–256) |
| `MORPH_FCGI_RUNTIME_WORKERS` | _(none)_ | Legacy alias for the minimum worker count |
| `MORPH_FCGI_RUNTIME_QUEUE_MAX` | `256` | Maximum turns waiting for a runtime |
| `MORPH_FCGI_RUNTIME_WAIT_SECONDS` | `600` | Maximum runtime-pool wait time |
| `MORPH_FCGI_RUNTIME_IDLE_SECONDS` | `300` | Elastic-worker idle lifetime; `0` disables shrinking |
| `MORPH_FCGI_CONFIG` | `$HOME/.morph/config.toml` | Path to config file |
| `MORPH_FCGI_OUTPUT_DIR` | _(from config)_ | Override artifact output directory |
| `MORPH_FCGI_LOG_FILE` | _(from config)_ | Override log file path |
| `MORPH_FCGI_TRUST_HDR` | _(none)_ | Trusted proxy identity header |
| `MORPH_FCGI_ALLOW_SIGNUP` | _(none)_ | Set to `1` to enable public signup |
| `MORPH_FCGI_SIGNUP_CODE` | _(none)_ | Optional invite code for signup |

---

## Deployment

Examples in `web/deploy/`:

- `nginx.conf`         — fastcgi_pass + a separate location block for SSE
                          with `fastcgi_buffering off`, `fastcgi_read_timeout 1h`;
                          passes `HTTP_AUTHORIZATION` to FastCGI
- `morph-fastcgi.service` — hardened systemd unit
- `Dockerfile`         — multi-stage debian:bookworm-slim build

### Docker quick start

```bash
# Build
docker build -t morph-fastcgi -f web/deploy/Dockerfile .

# Run (port 9000 → container 80)
docker run -d --name morph -p 9000:80 \
  -v ~/.morph/config.toml:/home/morph/.morph/config.toml:ro \
  -v ~/.morph/skills:/home/morph/.morph/skills:ro \
  -v ~/.morph/exts:/home/morph/.morph/exts:ro \
  -v ~/.morph/output:/var/lib/morph/output \
  -v ~/.morph/log:/var/lib/morph/log \
  morph-fastcgi

# Ensure the container user (uid 999) can write to output and log dirs
chmod 777 ~/.morph/output ~/.morph/log
```

Environment variables `MORPH_FCGI_CONFIG`, `MORPH_FCGI_OUTPUT_DIR`, and
`MORPH_FCGI_LOG_FILE` are set in the Dockerfile to point to the container
paths. Config path values with `~` are expanded at load time, so
`output_dir = "~/.morph/output"` resolves correctly inside the container.

---

## Multi-process consistency

Both `morph` (CLI) and `morph-fastcgi` open the same SQLite DB with WAL +
`busy_timeout=5000`.  Writers serialise; readers don't block.  Within a
single FastCGI process the pthread cond-var is broadcast on every event
write so all SSE workers wake up immediately.  Across processes, the
worst-case latency is one `events_wait_after` poll (default 15 s, also the
heartbeat cadence).

---

## Runtime integration

The FastCGI process owns an elastic pool of independent runtimes. It prewarms
`MORPH_FCGI_RUNTIME_MIN_WORKERS`, creates replicas on demand up to
`MORPH_FCGI_RUNTIME_MAX_WORKERS`, and reclaims elastic replicas after the idle
timeout. Different sessions execute in parallel; turns for the same session
are serialized to preserve history order. Once the maximum is reached, turns
enter a bounded queue instead of failing with `EBUSY`. Each turn binds its
authenticated user identity and session visibility into the selected tool
runtime, so memory and credit queries remain tenant-scoped. Shutdown waits for
accepted turn jobs before closing replicas and then the primary runtime.

`GET /api/health` exposes `runtime_pool.workers`, `busy`, `starting`, `waiting`,
`min`, `max`, `queue_max`, and `idle_seconds` for autoscaling and alerting.
