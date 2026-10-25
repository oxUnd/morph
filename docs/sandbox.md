# Sandbox policy

This document describes the operating-system sandbox used by the `exec` tool and
executable extensions. The policy is capability based: filesystem paths,
network access, PTYs, process inspection, IPC, environment variables, and
resource ceilings are independent permissions.

## Explicit host execution

`exec` defaults to sandboxed child execution. For a trusted development setup,
the user config can opt in to individual host calls:

```toml
[exec]
allow_host_execution = true
```

The caller must additionally pass `host: true` to `exec`. Opting in does not
change ordinary calls, does not automatically retry failed commands on the
host, and does not change executable-extension isolation. Command parsing and
approval still apply; the approval reason identifies host execution. The initial
result reports `execution_mode: "host"` or `"sandbox"`. Host children close
inherited descriptors and retain process groups, output limits, timeouts, and
`process` session control.

Host execution does not apply the child OS sandbox, including its filesystem
and network restrictions or resource limits. It has the current user's host
access. Enable it only for trusted tasks that need this access; it is not a
narrow Apple Events entitlement. A `host: true` call is rejected when the config
option is disabled, which is the default.

On macOS this supports AppleScript automation that `System Events` rejects from
a Seatbelt-sandboxed sender even after profile rules allow Apple Events. It does
not grant or reset macOS Automation, Accessibility, or Screen Recording consent.
Grant the required access to the actual responsible terminal/application in
System Settings > Privacy & Security. A denied system permission remains denied.

Example explicit tool arguments:

```json
{"command":"osascript -e 'tell application \"System Events\" to get name of first application process whose frontmost is true'","host":true,"timeout_ms":10000}
```

Offline behavior is covered by `ExecToolTest` and `ProcessTest`. The opt-in live
test requires an unlocked Mac with existing privacy consent:

```sh
MORPH_TEST_HOST_AUTOMATION=1 build/bin/morph-tests --gtest_filter=ExecToolTest.LiveHostAutomationReadsFrontmostApplication
```

## Security invariants

1. macOS Seatbelt starts from an explicit allowlist. Linux seccomp starts from
   an allow-by-default denylist that rejects only sandbox-escape syscalls, and
   Landlock confines filesystem paths.
2. A failed sandbox initialization must stop the child. Executable extensions
   exit with status 126 and are never executed without isolation.
3. Write and delete are separate filesystem capabilities.
4. PTY access never implies general device access. Only the PTY allocator and
   its slave devices receive read, write, and ioctl access.
5. macOS runtime process inspection follows host permissions. Linux defaults to
   self-only `/proc`; explicit `process_info` follows the host's `/proc` DAC
   because morph does not create a PID namespace.
6. macOS runtime Mach lookup and registration support host applications.
7. Temporary-directory access is explicit. The exec frontend additionally
   grants built-in tool cache paths; other sandbox callers do not inherit them.
8. Child processes close all inherited file descriptors above stderr before
   entering the sandbox. Path policies cannot revoke access through a file
   descriptor that was opened by the parent.
9. Child processes start a new terminal session after replacing stdin, stdout,
   and stderr with controlled pipes or `/dev/null`. This prevents an inherited
   controlling terminal from becoming an escape channel through terminal
   ioctls such as `TIOCSTI`.

## Executable extension manifest

Use named capabilities in `manifest.toml`:

```toml
sandbox_capabilities = [
  "network",
  "exec",
  "pty",
  "process_info",
  "ipc",
  "temporary_directory",
]
```

Available names:

| Capability | Effect |
| --- | --- |
| `network` | Direct socket networking |
| `filesystem` | Write access to the required `allowed_paths` list |
| `exec` | Execute child programs and use their required runtime paths |
| `environment` | Preserve the full environment when `allowed_env` is empty |
| `pty` | Allocate pseudoterminals and perform PTY ioctls |
| `process_info` | Same-sandbox inspection on macOS; full host-DAC `/proc` view on Linux |
| `ipc` | Use POSIX semaphores and shared memory |
| `temporary_directory` | Write and delete below the OS temporary directories |

The numeric `permissions` field remains accepted for compatibility:

| Bit | Value | Named replacement |
| --- | ---: | --- |
| `EXT_PERM_NETWORK` | 1 | `network` |
| `EXT_PERM_FILESYS` | 2 | `filesystem` |
| `EXT_PERM_EXEC` | 4 | `exec` |
| `EXT_PERM_ENV` | 8 | `environment` |
| `EXT_PERM_PTY` | 16 | `pty` |
| `EXT_PERM_PROCESS_INFO` | 32 | `process_info` |
| `EXT_PERM_IPC` | 64 | `ipc` |
| `EXT_PERM_TEMP` | 128 | `temporary_directory` |

New manifests should use names. Unknown names are rejected during manifest
loading instead of being silently ignored. A named `filesystem` capability
without `allowed_paths` is rejected. Only the legacy numeric bit retains the
old unrestricted-write behavior when no paths are present.

On macOS, an extension may add exact Mach service names:

```toml
allowed_mach_services = [
  "com.apple.locationd.desktop.registration",
  "com.apple.locationd.desktop.synchronous",
]
```

Service names accept only ASCII letters, digits, `.`, `_`, and `-`. This field
does nothing on Linux.

## `exec` defaults

`exec` enables the capabilities needed by normal developer tools:

- child execution;
- PTY allocation;
- platform-scoped process inspection as described below;
- POSIX IPC.

System paths are granted automatically so toolchains resolve their runtime,
while writes and deletion elsewhere remain limited to the workdir, output
directory, the active permission profile, and approved additional paths:

```text
read-only:      /usr  /bin  /sbin  /System  /Library  /opt/homebrew  /private
read/write/del: /tmp
```

The command workdir receives read, write, and delete rights. When the workdir
contains a `.git` file that points elsewhere (a git worktree), that git
directory is granted write and delete access explicitly because it lives
outside the workdir.

Exec adds shared directories and toolchain paths from
`src/sandbox/tool_paths.c` on both macOS and Linux. Directory rules receive
read, write, and delete permissions, including executable installation,
configuration updates, atomic replacement, and lock cleanup. The parent creates
missing directories before spawning the sandboxed process. Supported absolute
environment overrides are resolved before environment filtering. GOPATH,
JULIA_DEPOT_PATH, R_LIBS_USER, CONDA_PKGS_DIRS, and CONDA_ENVS_PATH accept
colon-separated lists; empty and relative entries are skipped. Paths resolving
to HOME or its ancestors are not granted. Unavailable directories are skipped.

Runtime startup registers the same resolved presets with tool-context path
approval even when exec is disabled. An explicit `skill.dir` is included too. Reads, writes, deletions, and explicit `request_permissions` requests
inside these roots do not prompt again. Built-in paths use a dynamic collection
independent of user grant limits. Exec merges all matching scoped, persistent,
profile, and built-in grants dynamically; bounded compatibility APIs report
`ENOSPC` instead of silently dropping paths. Profile write/delete roots are
recognized by native tools as well. Native write and delete session grants are
separate. `request_permissions` grants read, write, and delete access to directories
and their descendants, shared by native tools and all command executables. Its
optional `command` provides context only. Grants expire at the end of the turn
(default) or session (`scope=session`); choosing Always persists the directory
grant for the current project. Write does not include deletion: installers and
atomic replacements can require both. Paths outside the approved roots,
including symlinks leading outside, remain restricted. Legacy executable-scoped
grants retain their original scope; request directory access again to share it
across tools. Command execution approvals remain separate from filesystem access.

Valid shell scripts may contain variable-expanded command names, substitutions,
functions, arrays, loops, and assignment-only statements. AST analysis still
collects known program names; runtime-expanded commands additionally require the
`shell` program permission outside trusted command scopes. The full script is
shown for approval. Workspace and temporary-directory commands retain their
existing automatic approval behavior; syntax errors remain rejected.

The built-in families include:

- Shared XDG cache/config/data/state roots, `~/.local/bin`, `~/.local/lib`,
  `~/.local/include`, `~/.local/etc`, and existing `XDG_RUNTIME_DIR`.
- Skills in `~/.morph/skills` and `~/.agents/skills`; other files in their
  parent directories do not inherit these grants.
- Codex, Claude Code, Gemini CLI, Continue, Cursor, Aider, OpenCode, Ollama,
  LM Studio, GPT4All (macOS), Hugging Face, ModelScope, PyTorch, vLLM, Triton,
  and explicit tokenizer/model-cache overrides. Full AI tool directories
  include sessions, configuration, credentials, and installed skills. Project
  skill directories are already covered by their workspace permissions.
- Node/npm/pnpm/Yarn/Corepack, Bun, Deno, nvm, Volta, fnm, and user npm prefixes.
- Cargo/rustup, Go workspaces and installed tools, Gradle/Maven/Ivy/SBT/SDKMAN,
  .NET/NuGet, Python/uv/pipx/pyenv/Conda/Mamba, Ruby/Bundler/rbenv/RVM,
  Composer, Julia, R, Haskell, Elixir, Clojure, Dart, and Android SDK/AVD.
- Conan/vcpkg/xmake/CMake, asdf/mise, build and compiler caches, browser testing,
  Docker Buildx state, editor extension directories, TeX, and model caches.
- HTTPie, aria2, lftp, SSH control sockets in `~/.ssh/controlmasters`, Helm and
  kubectl caches, Pulumi, Ansible, and an explicit Terraform plugin cache.
- On macOS, `~/Library/Caches`, `~/Library/Python`, `~/Library/R`, `~/Library/pnpm`,
  `~/Library/Android/sdk`, selected fnm/Coursier Application Support directories,
  SwiftPM state, `~/Library/Developer` (CoreSimulator, Xcode archives,
  DerivedData and device support), `/Library/Developer`, provisioning profiles
  under `~/Library/MobileDevice`, Xcode Application Support and device logs.

`~/.cache`, `~/.config`, `~/.local/share`, and `~/.local/state` remain writable
alongside absolute XDG overrides. Whole toolchain directories include any
configuration and credentials stored inside them. No whole HOME or Library
permission is implied. Rules are built in; there is no extra configuration.

`/dev/tty` receives read/write access without deletion rights. `/tmp`,
`/var/tmp`, and an existing absolute `$TMPDIR` follow the temporary-directory
capability on both platforms. A custom TMPDIR resolving to HOME, its ancestors,
or `/` is ignored. macOS also grants its native per-user temporary directory.

Other home paths, system installation prefixes, SSH keys/known_hosts, and daemon
sockets still need explicit grants. In particular, a directory preset does not
make root-level files such as `~/.npmrc` or `~/.wget-hsts` writable. Network,
listener, and privileged socket capabilities remain independent of filesystem
permissions. The source table is the authoritative list of supported paths and
environment overrides; arbitrary project/tool configuration is not executed to
discover additional directories.

Direct networking is off by default and controlled by `[exec].network`:

```toml
[exec]
network = false
```

Sandbox resource ceilings are applied by the sandbox layer rather than the
`[exec]` table. The memory value applies to `RLIMIT_DATA`. `RLIMIT_AS` is
skipped for executable workloads because modern runtimes reserve large virtual
address ranges. Core dumps remain disabled. CPU time follows the command
timeout. Resource limits are best-effort OS controls: unsupported limits are
logged, while failure of the filesystem or syscall sandbox remains fatal.

## Local development runtime

The exec frontend enables a built-in development runtime, independently of
extension capabilities. On macOS this permits IOKit operations for GPU/Metal
and IOSurface access, System V IPC, user preferences, distributed notification
posting, device ioctls and signals to processes accessible to the current user.
Filesystem paths and network policy continue to apply. Other sandbox callers
retain the baseline policy unless they explicitly enable this runtime.

Exec also enables IPC and inherits the host environment, including Android,
Java, Xcode and simulator settings. Environment inheritance includes credentials
present in the parent process. macOS privacy consent, code-signing entitlements,
Keychain authorization and administrator privileges are still enforced by the OS.

Background exec sessions default to no timeout. An explicit `timeout_ms` still
limits them; foreground commands retain the configured default. Background
sessions remain owned by the process manager and stop when it shuts down.

## macOS policy

The Seatbelt profile grants only selected system runtime paths. In particular,
it does not grant the whole `/private/var` tree. Baseline Mach lookups and
read-only sysctls are allowed. File metadata reads are allowed so
processes can traverse parent directories and resolve an approved descendant;
file contents and directory enumeration remain controlled by the path rules.
The narrow `RootDomainUserClient` and `kern.grade_cputype` rules follow the
Chromium/Codex compatibility baseline used by Java and CPU detection.

PTY support consists of:

```scheme
(allow pseudo-tty)
(allow file-read* file-write* file-ioctl (literal "/dev/ptmx"))
(allow file-ioctl (regex #"^/dev/ttys[0-9]+"))
```

Slave read/write access additionally requires the sandbox-issued
`com.apple.sandbox.pty` extension.

`sandbox_init()` and `sandbox-exec` are deprecated Apple APIs, but they remain
the available fine-grained Seatbelt interface for a CLI. A process already
inside another Seatbelt sandbox generally cannot apply a second profile and
receives `EPERM`. An inner profile cannot relax an outer profile.

Consequences:

- run macOS sandbox end-to-end tests from an ordinary terminal or an approved
  unsandboxed test executor;
- do not treat nested `EPERM` as a product policy denial;
- do not skip sandboxing after nested initialization fails.

## Linux policy

Linux combines two layers:

- seccomp-BPF denylists escape-relevant system calls, with a default ALLOW
  action;
- Landlock limits filesystem paths and, on ABI 5 or newer, device ioctls.

The seccomp filter deliberately allows unknown syscalls. A syscall allowlist
cannot keep pace with the kernel and breaks runtimes such as Node.js, Python,
Go, and glibc whenever they issue a syscall that is not listed. Denied
operations return `EPERM` so callers degrade gracefully instead of being
killed. Only operations that would let the sandbox escape are denied.

The `ioctl` syscall remains available because terminals and common runtimes use
it, but Landlock ABI 5 denies ioctl on newly opened device files. With the
`pty` capability, only `/dev/ptmx` and `/dev/pts` receive device ioctl access.
Older Landlock ABIs still receive PTY read/write access but cannot scope device
ioctls; morph logs this kernel limitation. ABI-dependent filesystem rights are
masked before ruleset creation, so ABI 1 and 2 kernels do not receive the newer
`REFER` or `TRUNCATE` bits. The `ipc` capability grants the `/dev/shm` access
and seccomp calls needed by POSIX shared memory.

Linux Landlock only attaches device-ioctl restrictions when a device is newly
opened. Morph therefore starts a new terminal session and closes inherited
descriptors before applying Landlock; this also prevents accidental inheritance
of database, log, socket, or TTY handles from the parent process.

Executable workloads without `process_info` receive only `/proc/self`,
`/proc/thread-self`, and a small set of CPU, memory, mount, and kernel identity
files. Full `/proc` reads require the explicit capability. On kernels older
than Landlock ABI 6, Landlock cannot scope signals to the sandbox domain; this
kernel limitation does not expand filesystem access, but it remains a platform
constraint for executable workloads that need to signal child processes.

## Validation

Run normal tests:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The PTY regression test verifies `posix_openpt`, `grantpt`, and `unlockpt`
inside the sandbox. Linux-only tests also cover POSIX shared memory and both
sides of the `/proc` `process_info` policy. Session and inherited-descriptor
tests protect the pre-sandbox child setup. The filesystem tests separately
verify write and delete rights. Network tests cover denied and allowed loopback
connections. Manifest tests reject unknown named capabilities.

On macOS, if every sandbox test fails with exit 126 or
`sandbox_apply: Operation not permitted`, first check whether the test runner
is itself sandboxed. Run this control outside that runner:

```sh
python3 -c 'import pty,os; m,s=pty.openpty(); print(os.ttyname(s))'
```

References:

- [OpenAI Codex macOS Seatbelt base policy](https://github.com/openai/codex/blob/main/codex-rs/sandboxing/src/seatbelt_base_policy.sbpl)
- [Chromium macOS sandbox design](https://chromium.googlesource.com/chromium/src/+/main/sandbox/mac/README.md)
- [Linux Landlock userspace API](https://www.kernel.org/doc/html/latest/userspace-api/landlock.html)
- [Bubblewrap sandboxing notes](https://github.com/containers/bubblewrap#sandboxing)
- [seccomp(2)](https://man7.org/linux/man-pages/man2/seccomp.2.html)
