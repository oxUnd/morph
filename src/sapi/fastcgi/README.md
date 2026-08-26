# morph-fastcgi

A FastCGI front-end for the Morph terminal-native agent — letting you put
nginx (or any FastCGI-aware server) in front of Morph and serve a Web UI
without bringing an HTTP stack into the core binary.

`morph-fastcgi` speaks the FastCGI protocol, not HTTP. A browser or `curl`
cannot connect to it directly; use nginx/Caddy/Apache in production, or the
`cgi-fcgi` command for a local protocol-level smoke test. Docker is optional.

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

## Prerequisites and build

The project vendors cJSON; do not install a separate cJSON development
package. Besides libfcgi, the normal Morph dependencies are also required.

### Debian/Ubuntu

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config \
  libfcgi-dev libsqlite3-dev libcurl4-openssl-dev libvips-dev \
  libcairo2-dev libpango1.0-dev libharfbuzz-dev libfreetype-dev

cmake -S . -B build -DBUILD_FASTCGI=ON
cmake --build build -j
```

### macOS/Homebrew

```bash
brew install cmake pkgconf sqlite curl fcgi vips cairo pango harfbuzz freetype

cmake -S . -B build -DBUILD_FASTCGI=ON
cmake --build build -j
```

The binary lands at `build/bin/morph-fastcgi`.

For a normal prefix install, including Morph's runtime data:

```bash
cmake --install build --prefix "$(brew --prefix)"
command -v morph-fastcgi
```

For development, where every rebuild should immediately update the command,
link the build artifact instead:

```bash
ln -sf "$PWD/build/bin/morph-fastcgi" \
  "$(brew --prefix)/bin/morph-fastcgi"
```

This is a local Homebrew-prefix installation, not a published Homebrew formula.

## Configure

FastCGI and CLI use the same TOML configuration. Create it before launch and
provide model credentials through environment variables; never put API keys in
the TOML file.

```bash
mkdir -p "$HOME/.morph/log" "$HOME/.morph/output" \
  "$HOME/.morph/skills" "$HOME/.morph/exts"
cp config.toml.example "$HOME/.morph/config.toml"

# Must match api_key_env in config.toml.
export OPENAI_API_KEY="..."
```

At minimum, review `[model.text]` in `~/.morph/config.toml`. Skills are
discovered from `~/.morph/skills/` and `~/.agents/skills/`. Extensions are
loaded from `~/.morph/exts/` when their manifest supports the `fastcgi` front.

MCP servers are configured in the same file:

```toml
[[mcp.servers]]
name = "filesystem"
transport = "stdio"
command = "npx"
args = ["-y", "@modelcontextprotocol/server-filesystem", "/allowed/path"]
auto_connect = true
```

FastCGI eagerly connects servers with `auto_connect = true`. Each runtime owns
its own MCP client and tool registry, so a stdio MCP server may have one child
process per warm/active runtime. Include that cost when setting runtime limits.

---

## Run without Docker

For local development, use writable paths under the current user's home and a
Unix socket under `/tmp`:

```bash
export MORPH_FCGI_LISTEN="unix:/tmp/morph-fastcgi.sock"
export MORPH_FCGI_DB="$HOME/.morph/data.db"
export MORPH_FCGI_CONFIG="$HOME/.morph/config.toml"
export MORPH_FCGI_OUTPUT_DIR="$HOME/.morph/output"
export MORPH_FCGI_LOG_FILE="$HOME/.morph/log/fastcgi.log"
export MORPH_FCGI_WORKERS=128
export MORPH_FCGI_RUNTIME_MIN_WORKERS=8
export MORPH_FCGI_RUNTIME_MAX_WORKERS=64

morph-fastcgi
# Or, without installing: ./build/bin/morph-fastcgi
```

`MORPH_FCGI_LISTEN` accepts `unix:/path/sock`, `:9000`, or `127.0.0.1:9000`.
Stop with `SIGINT` or `SIGTERM`; shutdown stops accepting requests, waits for
accepted turn jobs, then closes elastic replicas and the primary runtime.

For a protocol-level health check, install/use the `cgi-fcgi` program shipped
with libfcgi and run:

```bash
SCRIPT_NAME=/api/health \
DOCUMENT_URI=/api/health \
REQUEST_URI=/api/health \
REQUEST_METHOD=GET \
cgi-fcgi -bind -connect /tmp/morph-fastcgi.sock
```

The response should contain `Status: 200` and a JSON body with `status: "ok"`.
If `setup_required` is true, create the first administrator through the HTTP
endpoint after putting nginx in front of the socket.

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

### End-to-end HTTP example

The following assumes nginx is serving the supplied configuration on
`http://127.0.0.1` and that `jq` is installed. Run installation only once.

```bash
export MORPH_BASE_URL="http://127.0.0.1"
export MORPH_ADMIN_USER="admin"
export MORPH_ADMIN_PASSWORD="replace-with-a-strong-password"

curl -sS "$MORPH_BASE_URL/api/health" | jq

curl -sS -X POST "$MORPH_BASE_URL/api/install" \
  -H 'Content-Type: application/json' \
  --data "{\"username\":\"$MORPH_ADMIN_USER\",\
\"password\":\"$MORPH_ADMIN_PASSWORD\"}" | jq

MORPH_LOGIN_JSON=$(curl -sS -X POST "$MORPH_BASE_URL/api/login" \
  -u "$MORPH_ADMIN_USER:$MORPH_ADMIN_PASSWORD")
export MORPH_TOKEN=$(printf '%s' "$MORPH_LOGIN_JSON" | jq -r '.token')

MORPH_SESSION_JSON=$(curl -sS -X POST "$MORPH_BASE_URL/api/sessions" \
  -H "Authorization: Bearer $MORPH_TOKEN" \
  -H 'Content-Type: application/json' \
  --data '{"name":"browser session","model":"default"}')
export MORPH_SESSION_ID=$(printf '%s' "$MORPH_SESSION_JSON" | jq -r '.id')
```

Keep the SSE stream open in one terminal:

```bash
curl -N "$MORPH_BASE_URL/api/sessions/$MORPH_SESSION_ID/events" \
  -H "Authorization: Bearer $MORPH_TOKEN"
```

Submit a turn from another terminal. The request returns `202` immediately;
progress, the final answer, pool errors, and completion arrive through SSE.

```bash
curl -sS -X POST \
  "$MORPH_BASE_URL/api/sessions/$MORPH_SESSION_ID/turns" \
  -H "Authorization: Bearer $MORPH_TOKEN" \
  -H 'Content-Type: application/json' \
  --data '{"prompt":"List the tools currently available."}'
```

SSE connections last at most one hour, send a heartbeat every 15 seconds, and
support reconnection with `Last-Event-ID`.

### Common responses

| Status/event | Meaning |
|--------------|---------|
| HTTP 202 | Turn accepted; execution continues asynchronously |
| HTTP 429 `quota_exceeded/turns` | User daily turn quota exhausted |
| HTTP 429 `quota_exceeded/concurrent_turns` | User concurrency quota exhausted |
| HTTP 503 `shutting_down` | Process is no longer accepting turn jobs |
| SSE `error` with `Resource temporarily unavailable` | Runtime wait queue was full |
| SSE `error` with `Operation timed out` | No runtime became available before the wait deadline |

The built-in admin quota allows four concurrent turns; the free-user profile
allows one. These per-user quotas are separate from the process-wide runtime
pool limits.

### Agent capabilities

FastCGI uses the same runtime bootstrap as the CLI. At process startup it
discovers Skills, loads extensions that support the `fastcgi` front, registers
scheduled tasks and sub-agent tools, and connects configured MCP servers with
`auto_connect = true`. Every runtime has an independent tool registry and MCP
client set; replicas created during scale-out perform the same discovery and
registration before becoming available to requests.

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
| `MORPH_FCGI_WORKERS` | `128` | FastCGI connection slots (1–512); every open SSE stream occupies one |
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

There are two separate kinds of workers:

- `MORPH_FCGI_WORKERS` accepts FastCGI connections. Long-lived SSE streams
  occupy these threads, but they do not consume an agent runtime while idle.
- `MORPH_FCGI_RUNTIME_*_WORKERS` controls simultaneous model/ReAct execution.
  A runtime is a comparatively heavy independent agent instance.

A local macOS measurement was approximately 31 MB RSS with one runtime and
170 MB with eight runtimes, or roughly 20 MB for each additional runtime.
Models, Skills, extensions, MCP clients, allocator behavior, and platform
libraries can substantially change this number. Stdio MCP child processes are
not included in the Morph process RSS and must be budgeted separately.

Suggested starting points:

| Deployment | Connection workers | Runtime min/max | Queue |
|------------|-------------------:|----------------:|------:|
| Developer laptop | 32 | 1 / 4 | 32 |
| Small shared server | 128 | 4 / 32 | 256 |
| Larger host | 256 | 8 / 64 | 512 |

Treat these as initial values, not universal limits. Keep enough connection
workers for all expected SSE clients plus short API requests, then size runtime
workers from available memory and the upstream model provider's rate limits.
Model latency naturally causes queued demand to accumulate.

The turn submission endpoint returns `202` before runtime acquisition. If the
runtime queue is full or its wait deadline expires, the failure is published as
an SSE `error` event for that session. It is not returned by the original HTTP
request.

`GET /api/health` exposes `runtime_pool.workers`, `busy`, `starting`, `waiting`,
`min`, `max`, `queue_max`, and `idle_seconds` for autoscaling and alerting.
