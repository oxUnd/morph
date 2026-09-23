# Runtime prompt contexts

`agent/prompt_context` builds an ordered set of runtime-context providers once
per `react_run`. The default provider collects and renders `environment_context`.
A provider has a stable name and a collection/render callback. Declaration order
is serialization order; duplicate provider names are rejected. Add new providers
to the default array without changing the static prompt builder.

`environment_context_collect` takes borrowed execution settings and stores its
snapshot in the turn arena. `environment_context_render` is pure: it normalizes
aliases and writes XML to a `morph_buf_t` without inspecting the machine. There
is no process-wide snapshot cache or separate per-field heap ownership.

The exec tool exposes its effective working directory and the process manager's
actual shell through `tool_spec.get_environment`. Sub-agent tool copies preserve
this callback. Without an exec tool, collection uses the agent's working
directory (or native current directory) and the inherited shell. Per-command
working-directory overrides do not change the default working directory for
later commands or turns.

The fixed field order is `cwd`, `shell`, `os`, `arch`, `current_date`, `timezone`,
`git_branch`, `git_root`, `sandbox`. Missing and empty values are omitted. XML
metacharacters and quotes are escaped, embedded line breaks use character
references, and malformed UTF-8 and XML-illegal control characters are dropped.
Only explicitly selected metadata is read; the environment is never enumerated.

Date is the local calendar date, with no time of day. POSIX timezone detection
uses an installed IANA `TZ` name or the `/etc/localtime` zoneinfo link. When no
reliable IANA identity is available, the local UTC offset is used. Windows uses
native directory/architecture APIs and the CRT local date/offset; no Windows to
IANA mapping database is bundled.

Git discovery uses bounded, nonblocking regular-file reads, not subprocesses.
It supports ordinary repositories, nested working directories, `.git` files
(including linked worktrees and submodules), unborn branches, and detached HEAD.
Repository root is separate from the actual working directory. Git dirty state
is deliberately excluded. Unsupported Git environment overrides cause Git fields
to be omitted rather than guessed; bare repositories are not reported. POSIX
repository discovery stops at filesystem boundaries. Reftable repositories and
nonstandard HEAD references may omit branch information.

Sandbox is emitted only when an explicit mode is supplied. Morph's exec tool
currently applies per-command capability/path policies, so it does not invent a
single global sandbox mode for this block.

For structured backends, request construction adds one transient system message
after the stable system prompt and before conversation messages. This prefix is
never added to active history, persisted history, or compaction input. It is
reapplied after history rebuilds and included in token-budget estimates. Legacy
text-only backends receive the same XML appended to the system prompt because
their interface cannot represent a separate system message. All model iterations
within a turn reuse the snapshot; the next turn recollects it. Turn completion
clears the snapshot pointer, and the turn arena reclaims its storage.
