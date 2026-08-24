# Sandbox policy

This document describes the operating-system sandbox used by `bash_exec` and
executable extensions. The policy is capability based: filesystem paths,
network access, PTYs, process inspection, IPC, environment variables, and
resource ceilings are independent permissions.

## Security invariants

1. The policy starts from deny-by-default on macOS Seatbelt and Linux seccomp
   plus Landlock.
2. A failed sandbox initialization must stop the child. Executable extensions
   exit with status 126 and are never executed without isolation.
3. Write and delete are separate filesystem capabilities.
4. PTY access never implies general device access. Only the PTY allocator and
   its slave devices receive read, write, and ioctl access.
5. macOS process inspection is limited to the same sandbox. Linux defaults to
   self-only `/proc`; explicit `process_info` follows the host's `/proc` DAC
   because morph does not create a PID namespace.
6. macOS Mach services are allowlisted by exact service name.
7. Temporary-directory access is explicit. Cache directories are not writable
   unless separately granted as filesystem paths.
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

## `bash_exec` defaults

`bash_exec` enables the capabilities needed by normal developer tools:

- child execution;
- PTY allocation;
- platform-scoped process inspection as described below;
- POSIX IPC.

Local mode additionally reads all files, enables direct networking, and uses
the OS temporary directory. Writes and deletion remain limited to the workdir,
output directory, temporary directory, active permission profile, and approved
additional paths.

Server mode does not enable direct networking or OS temporary-directory writes
unless its configured path policy grants them. Its read, write, delete,
environment, and network policy remains fixed by `[react.bash_exec_server]`.

Resource ceilings are configurable:

```toml
[react]
bash_exec_max_memory_mb = 2048
bash_exec_max_open_files = 1024
```

The memory value applies to `RLIMIT_DATA`. `RLIMIT_AS` is skipped for executable
workloads because modern runtimes reserve large virtual address ranges. Core
dumps remain disabled. CPU time follows the command timeout. Resource limits
are best-effort OS controls: unsupported limits are logged, while failure of
the filesystem or syscall sandbox remains fatal.

## macOS policy

The Seatbelt profile grants only selected system runtime paths. In particular,
it does not grant the whole `/private/var` tree. Baseline Mach lookups and
read-only sysctls are explicit allowlists. File metadata reads are allowed so
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

- seccomp-BPF allowlists system calls;
- Landlock limits filesystem paths and, on ABI 5 or newer, device ioctls.

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
