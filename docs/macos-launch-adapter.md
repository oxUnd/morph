# Bounded macOS app launch

The optional `scripts/macos_mcp_adapter.py` wraps the stdio MCP server from
macos-computer-use-kit 0.3.2. It adds `macos_open_app` and preserves the existing
tools. It uses Python 3.9 or later and the upstream toolkit; it does not patch
the toolkit or add a dependency to the C runtime.

Configure the existing macOS MCP server, using absolute paths appropriate to
your installation:

```toml
[[mcp.servers]]
name = "macos-computer-use"
transport = "stdio"
command = "/usr/bin/python3"
args = ["/opt/homebrew/share/morph/scripts/macos_mcp_adapter.py", "--", "/Users/you/.local/bin/uvx", "macos-computer-use-kit==0.3.2", "mcp"]
auto_connect = true
connect_timeout = 60
```

The script is installed under `${CMAKE_INSTALL_DATADIR}/morph/scripts`. In a
source checkout, its absolute source path also works. Restart the client to
reload the tool inventory. Installation of the toolkit and macOS permissions
remain the upstream server's responsibility.

For a launch-only request the model makes one call. Internally the adapter:

1. Launches the requested app through upstream `macos_app`.
2. Activates its returned PID through upstream `macos_app`.
3. Retrieves on-screen windows using that PID through `macos_window`.

`completed: true` requires verified activation and at least one on-screen
window owned by the resolved PID. It is evidence at the time of those checks,
not a guarantee against subsequent user focus changes or window occlusion.
The result includes all internal operations and observations in `steps`.
This reduces model decisions; it does not turn three underlying operations
into one OS operation or hide them.

A launch timeout permits one running-app lookup, accepting a unique exact
name, bundle ID, or bundle-tail match. Other failures return immediately.
There are no screenshot, OCR, focus, click, or input fallbacks. An upstream
policy refusal, including `screen_locked` and its retry policy, is preserved.
Dry-run results never claim completion. An app without an on-screen window
does not satisfy this visible-window operation.

For older conversations that still choose low-level `macos_app activate`, a
successful verified activation is enriched with one read-only window query.
Its `foreground_check.completed` confirms the same narrow foreground/window
state. This check is reported in `foreground_check.observation`; it does not
mark a broader UI task complete. Failed activations receive no extra probe.

The extra tool is advertised only when the upstream inventory exposes both
required tools and marks `macos_app` as mutating. Read-only and tool-exclusion
settings therefore do not acquire a new write capability. The wrapper speaks
the upstream server's sequential newline-delimited JSON-RPC protocol; it is
not a generic proxy for servers that initiate client-side requests.

Run offline contract tests with:

```sh
python3 tests/test_macos_mcp_adapter.py
```

These tests cover PID-based localized targeting, launch-timeout resolution,
lock refusal, dry runs, missing windows, and upstream tool restrictions.
Actual launch and model stopping behavior require a live Mac session.
