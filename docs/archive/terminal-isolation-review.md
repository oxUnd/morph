# Terminal isolation review (2026-11-12)

This incident and follow-up list describe the subprocess implementations reviewed on
2026-11-12. For the current process API, see [Asynchronous commands](../async-exec.md).

### Incident and completed fix

Session `a93b46f2` ran `zsh -ic 'which bun && bun --version'` during dependency
installation at 16:46:32 (Asia/Shanghai). A pipe-mode command had a separate
process group but retained the CLI's controlling terminal. The interactive shell
left that command's group in the foreground; a later CLI input read triggered
`SIGTTIN`, producing `zsh: suspended (tty input) morphctl run dev`.

`spawn_pipe_process()` now starts an isolated OS session and waits for group
creation before publishing the process, preserving immediate cancellation.
Regression coverage checks foreground-group preservation for pipe and PTY modes,
plus immediate process-group termination. The fix was built and the relevant
process, execution, hook and CLI job checks passed locally; two existing tests
were skipped because their environment prerequisites were unavailable.

### Open items

| Path | Evidence and remaining work |
| --- | --- |
| External guardrail: `src/agent/guardrail.c`, external rule spawn | Uses `POSIX_SPAWN_SETPGROUP` without session isolation. A bounded PTY probe using the same spawn setup and a shell wrapper running `zsh -fic` reproduced foreground-group theft. Isolate noninteractive rules and verify protocol input, timeout and cancellation. The full guardrail protocol path was not exercised by this probe. |
| MCP stdio: `src/mcp/mcp_stdio.c`, `mcp_stdio_spawn()` | Inherits the CLI's process group and controlling terminal despite redirected standard streams. A server can still open `/dev/tty`; group-directed signals can affect the CLI. The tested shell wrapper restored the foreground group, so the original theft was not reproduced on this path. Define session isolation and descendant shutdown together; disconnect currently signals only the direct server PID. |
| Video preview: `src/render/video.c`, `video_extract_preview()` | Redirects stdout/stderr but inherits stdin and does not pass ffmpeg `-nostdin`. Potential competition for composer input; input loss has not been dynamically reproduced. Disable interactive input and add a terminal-input regression. |
| Extension installation: `src/ext/install.c`, `run_cmd()` / `run_cmd_capture()` | Build and Git subprocesses inherit the controlling terminal and process group; stdin is also inherited. Review noninteractive build isolation and Git credential prompting. The tested shell wrapper did not reproduce foreground-group theft; input and signal interference remain audit findings. |

### Repair constraints and acceptance

- Treat process groups and OS sessions separately: redirecting stdin or calling
  `setpgid()` alone does not detach the controlling terminal.
- Preserve intentional terminal users. `img_annotate` pauses the CLI and launches
  the editor with terminal access; interactive playback needs its own ownership
  decision. Do not blindly detach every child.
- Dynamic-tool execution and extension execution already call
  `sandbox_start_isolated_session()`. Nested QuickJS `morph.exec()` therefore has
  upstream isolation in the normal runtime path; its process-group-only spawn is
  not evidence of the same CLI exposure.
- Exercise real entry points in a PTY: compare the owner's foreground group
  before/after interactive descendants; ensure drafts survive background work;
  verify timeout, immediate cancellation, descendant cleanup and protocol EOF.
- Preserve permission decisions and explicit terminal handoff contracts. Run
  relevant behavior tests and `source_control_flow_conventions`; run CLI layout
  and presentation checks if UI handling changes.
