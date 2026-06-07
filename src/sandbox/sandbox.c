#include "sandbox.h"
#include "util/array.h"
#include "util/error.h"
#include "util/log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

extern char **environ;

int sandbox_apply_rlimits(unsigned int permissions, int max_memory_mb,
			  int max_cpu_seconds, int max_file_size_mb,
			  int max_processes, int max_open_files)
{
	struct rlimit rl;

	if (max_memory_mb > 0) {
		/*
		 * When EXEC permission is granted, skip RLIMIT_AS.
		 * RLIMIT_AS limits the total virtual address space
		 * (mmap + brk), which breaks multi-process runtimes
		 * like Chromium that lazy-mmap large V8 CodeRange
		 * regions across several child processes.  RLIMIT_DATA
		 * still limits heap allocation via brk/sbrk.
		 */
		if (!(permissions & EXT_PERM_EXEC)) {
			rl.rlim_cur = (rlim_t)max_memory_mb * 1024 * 1024;
			rl.rlim_max = (rlim_t)max_memory_mb * 1024 * 1024;
#ifdef RLIMIT_AS
			if (setrlimit(RLIMIT_AS, &rl) != 0) {
				log_warn("sandbox: setrlimit RLIMIT_AS failed: %s",
					 strerror(errno));
			} else {
				log_info("sandbox: RLIMIT_AS set to %dMB",
					 max_memory_mb);
			}
#else
			log_info("sandbox: RLIMIT_AS not available on this OS");
#endif
		} else {
			log_info("sandbox: RLIMIT_AS skipped (EXT_PERM_EXEC set)");
		}
#ifdef RLIMIT_DATA
		rl.rlim_cur = (rlim_t)max_memory_mb * 1024 * 1024;
		rl.rlim_max = (rlim_t)max_memory_mb * 1024 * 1024;
		if (setrlimit(RLIMIT_DATA, &rl) != 0) {
			log_warn("sandbox: setrlimit RLIMIT_DATA failed: %s",
				 strerror(errno));
		}
#endif
	}

	if (max_cpu_seconds > 0) {
		rl.rlim_cur = (rlim_t)max_cpu_seconds;
		rl.rlim_max = (rlim_t)max_cpu_seconds + 1;
		if (setrlimit(RLIMIT_CPU, &rl) != 0) {
			log_warn("sandbox: setrlimit RLIMIT_CPU failed: %s",
				 strerror(errno));
		} else {
			log_info("sandbox: RLIMIT_CPU set to %ds",
				 max_cpu_seconds);
		}
	}

	if (max_file_size_mb > 0) {
		rl.rlim_cur = (rlim_t)max_file_size_mb * 1024 * 1024;
		rl.rlim_max = (rlim_t)max_file_size_mb * 1024 * 1024;
		if (setrlimit(RLIMIT_FSIZE, &rl) != 0) {
			log_warn("sandbox: setrlimit RLIMIT_FSIZE failed: %s",
				 strerror(errno));
		} else {
			log_info("sandbox: RLIMIT_FSIZE set to %dMB",
				 max_file_size_mb);
		}
	}

#ifdef RLIMIT_NPROC
	if (max_processes > 0) {
		rl.rlim_cur = (rlim_t)max_processes;
		rl.rlim_max = (rlim_t)max_processes;
		if (setrlimit(RLIMIT_NPROC, &rl) != 0) {
			log_warn("sandbox: setrlimit RLIMIT_NPROC failed: %s",
				 strerror(errno));
		} else {
			log_info("sandbox: RLIMIT_NPROC set to %d",
				 max_processes);
		}
	}
#else
	(void)max_processes;
#endif

	rl.rlim_cur = max_open_files > 0 ? (rlim_t)max_open_files : 256;
	rl.rlim_max = max_open_files > 0 ? (rlim_t)max_open_files : 256;
	if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
		log_warn("sandbox: setrlimit RLIMIT_NOFILE failed: %s",
			 strerror(errno));
	} else {
		log_info("sandbox: RLIMIT_NOFILE set to %d",
			 max_open_files > 0 ? max_open_files : 256);
	}

	/* Disable core dumps to avoid leaking sensitive memory */
	rl.rlim_cur = 0;
	rl.rlim_max = 0;
	if (setrlimit(RLIMIT_CORE, &rl) != 0) {
		log_warn("sandbox: setrlimit RLIMIT_CORE failed: %s",
			 strerror(errno));
	}

	(void)permissions;

	return 0;
}

/* ────────────────────────────────────────────────────────────────
 * Environment variable filtering
 *
 * Always-allowed minimal vars (needed for libc / dynamic loader):
 *   PATH, HOME, USER, LANG, LC_*, TZ, TMPDIR
 * Anything else is dropped unless EXT_PERM_ENV is set, or the var
 * name appears in allowed_env.
 * ──────────────────────────────────────────────────────────────── */

static int env_is_essential(const char *name)
{
	static const char *const essentials[] = {
		"PATH", "HOME", "USER", "LOGNAME", "SHELL",
		"LANG", "TZ", "TMPDIR", "PWD",
		NULL
	};
	for (int i = 0; essentials[i]; i++) {
		if (strcmp(name, essentials[i]) == 0)
			return 1;
	}
	if (strncmp(name, "LC_", 3) == 0)
		return 1;
	return 0;
}

static int env_in_allow_list(const char *name,
			     const char *const *allowed,
			     int count)
{
	if (!allowed)
		return 0;
	for (int i = 0; i < count; i++) {
		if (allowed[i] && strcmp(name, allowed[i]) == 0)
			return 1;
	}
	return 0;
}

int sandbox_apply_env(const char **allowed_env, int count,
		      unsigned int permissions)
{
	/* If ENV permission is set with no explicit allow list, leave
	 * the environment untouched — the caller has opted in. */
	if ((permissions & EXT_PERM_ENV) && (count <= 0 || !allowed_env)) {
		log_info("sandbox: env unrestricted (EXT_PERM_ENV set)");
		return 0;
	}

	/* Build a snapshot of variable names to remove. We can't iterate
	 * environ while calling unsetenv, since unsetenv mutates it. */
	int env_count = 0;
	while (environ && environ[env_count])
		env_count++;

	if (env_count == 0)
		return 0;

	char **names = calloc((size_t)env_count, sizeof(char *));
	if (!names)
		return -ENOMEM;

	int n_names = 0;
	for (int i = 0; i < env_count; i++) {
		const char *entry = environ[i];
		const char *eq = strchr(entry, '=');
		if (!eq)
			continue;
		size_t name_len = (size_t)(eq - entry);
		char *name = malloc(name_len + 1);
		if (!name)
			continue;
		memcpy(name, entry, name_len);
		name[name_len] = '\0';

		int keep = env_is_essential(name) ||
			   env_in_allow_list(name, allowed_env, count);
		if (keep) {
			free(name);
		} else {
			names[n_names++] = name;
		}
	}

	int removed = 0;
	for (int i = 0; i < n_names; i++) {
		if (unsetenv(names[i]) == 0)
			removed++;
		free(names[i]);
	}
	free(names);

	log_info("sandbox: env filtered (%d vars removed, %d allowed)",
		 removed, count);
	return 0;
}

/* ────────────────────────────────────────────────────────────────
 * Filesystem path restrictions
 * ──────────────────────────────────────────────────────────────── */

#ifdef __linux__

/*
 * Landlock LSM implementation (Linux 5.13+)
 *
 * Uses raw syscalls to avoid glibc version dependency.
 * Gracefully degrades: if the kernel doesn't support landlock,
 * we log a warning and continue with seccomp+rlimit only.
 *
 * Requires kernel headers >= 5.13 for <linux/landlock.h>.
 * Ubuntu 20.04 and older are NOT supported — use Ubuntu >= 22.04
 * or equivalent distro.
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <linux/landlock.h>

/* LANDLOCK_ACCESS_FS_TRUNCATE was added in Landlock ABI v3 (kernel 6.2).
 * Older kernel headers (e.g. Debian 12 / linux-libc-dev 6.1) lack it. */
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE       (1ULL << 14)
#endif

#ifndef O_PATH
#define O_PATH 0x200000
#endif

static int ll_create_ruleset(uint64_t fs_access)
{
	struct landlock_ruleset_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.handled_access_fs = fs_access;
	return (int)syscall(__NR_landlock_create_ruleset,
			    &attr, sizeof(attr), 0);
}

static int ll_add_rule(int ruleset_fd, int path_fd, uint64_t access)
{
	struct landlock_path_beneath_attr pb;
	memset(&pb, 0, sizeof(pb));
	pb.allowed_access = access;
	pb.parent_fd = path_fd;
	return (int)syscall(__NR_landlock_add_rule,
			    ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
			    &pb, 0);
}

static int ll_restrict_self(int ruleset_fd)
{
	return (int)syscall(__NR_landlock_restrict_self, ruleset_fd, 0);
}

int sandbox_apply_fs(const char **allowed_paths, int count,
		     unsigned int permissions)
{
	/*
	 * Probe for landlock support.
	 * Landlock ABI v4+ (kernel 6.6+) rejects rulesets with
	 * handled_access_fs=0, returning ENOMSG.  Use a non-zero
	 * access mask so the probe works across all ABI versions.
	 */
	int probe_fd = ll_create_ruleset(LANDLOCK_ACCESS_FS_READ_FILE);
	if (probe_fd < 0) {
		if (errno == EOPNOTSUPP || errno == ENOSYS) {
			log_err("sandbox: landlock not supported by kernel");
			return -ENOSYS;
		}
		int err = errno;
		log_err("sandbox: landlock probe failed: %s",
			strerror(err));
		MORPH_RETURN(-err);
	}
	close(probe_fd);

	/*
	 * Determine handled access rights.
	 *
	 * We always handle both read+execute and write in the ruleset,
	 * then grant access only via explicit per-path rules (whitelist).
	 *
	 * When EXT_PERM_EXEC is set, the child needs to read and execute
	 * system paths (/bin, /usr/lib, /etc, etc.) to function.  We add
	 * a built-in set of essential system paths with read+execute so
	 * that the sandbox is not overly restrictive, while still
	 * preventing reads from arbitrary user paths (home directories,
	 * data mounts, etc.).
	 *
	 * When EXT_PERM_EXEC is NOT set, no system path rules are added —
	 * only the caller-specified allowed_paths are readable.
	 *
	 * Write policy:
	 *   FILESYS + paths  → handle write, grant on allowed_paths
	 *   FILESYS, no paths → don't handle write (unrestricted)
	 *   No FILESYS        → handle write, no rules (default-deny)
	 */
	uint64_t read_access =
		LANDLOCK_ACCESS_FS_READ_FILE |
		LANDLOCK_ACCESS_FS_READ_DIR |
		LANDLOCK_ACCESS_FS_EXECUTE;

	uint64_t write_access =
		LANDLOCK_ACCESS_FS_WRITE_FILE |
		LANDLOCK_ACCESS_FS_MAKE_REG |
		LANDLOCK_ACCESS_FS_MAKE_DIR |
		LANDLOCK_ACCESS_FS_REMOVE_FILE |
		LANDLOCK_ACCESS_FS_REMOVE_DIR |
		LANDLOCK_ACCESS_FS_MAKE_SYM |
		LANDLOCK_ACCESS_FS_MAKE_CHAR |
		LANDLOCK_ACCESS_FS_MAKE_FIFO |
		LANDLOCK_ACCESS_FS_MAKE_BLOCK |
		LANDLOCK_ACCESS_FS_MAKE_SOCK |
		LANDLOCK_ACCESS_FS_REFER |
		LANDLOCK_ACCESS_FS_TRUNCATE;

	uint64_t handled = read_access | write_access;
	int needs_write_rules = 0;

	if (permissions & EXT_PERM_FILESYS) {
		if (count > 0 && allowed_paths) {
			needs_write_rules = 1;
		} else {
			/*
			 * FILESYS requested without path restrictions:
			 * don't handle write access in the ruleset,
			 * so writes are NOT restricted by landlock.
			 */
			handled &= ~write_access;
		}
	}
	/* else: no FILESYS, write_access stays in handled = default-deny */

	int ruleset_fd = ll_create_ruleset(handled);
	if (ruleset_fd < 0) {
		int err = errno;
		log_err("sandbox: landlock create_ruleset failed: %s",
			 strerror(err));
		MORPH_RETURN(-err);
	}

	/*
	 * Add rules for each caller-specified allowed path.
	 * All paths get read+execute.
	 * Paths also get write if needs_write_rules is set.
	 */
	uint64_t path_access = read_access;
	if (needs_write_rules)
		path_access |= write_access;

	for (int i = 0; i < count; i++) {
		if (!allowed_paths[i])
			continue;
		int fd = open(allowed_paths[i], O_PATH | O_CLOEXEC);
		if (fd < 0) {
			log_warn("sandbox: landlock: cannot open path '%s': %s",
				 allowed_paths[i], strerror(errno));
			continue;
		}
		if (ll_add_rule(ruleset_fd, fd, path_access) < 0) {
			log_warn("sandbox: landlock: add_rule for '%s' failed: %s",
				 allowed_paths[i], strerror(errno));
			close(fd);
			continue;
		}
		log_info("sandbox: landlock: allowed path '%s' "
			 "(access=0x%llx)", allowed_paths[i],
			 (unsigned long long)path_access);
		close(fd);
	}

	/*
	 * When EXT_PERM_EXEC is set, add built-in system path rules
	 * with read+execute so that the sandboxed process can actually
	 * execute programs.  Without these, landlock's default-deny
	 * blocks reading /bin/sh, dynamic linker, shared libraries, etc.
	 */
	if (permissions & EXT_PERM_EXEC) {
		static const char *const sys_paths[] = {
			"/bin",
			"/sbin",
			"/usr",
			"/lib",
			"/lib32",
			"/lib64",
			"/etc",
			"/dev",
			"/proc",
			"/sys",
			"/tmp",
			"/var/tmp",
			"/run",
			"/snap",
			"/opt",
			"/nix",
			NULL
		};
		for (int i = 0; sys_paths[i]; i++) {
			int fd = open(sys_paths[i], O_PATH | O_CLOEXEC);
			if (fd < 0)
				continue;
			if (ll_add_rule(ruleset_fd, fd, read_access) < 0) {
				log_warn("sandbox: landlock: system path "
					 "'%s' add_rule failed: %s",
					 sys_paths[i], strerror(errno));
			} else {
				log_info("sandbox: landlock: system path "
					 "'%s' (read+exec)", sys_paths[i]);
			}
			close(fd);
		}
	}

	/*
	 * PR_SET_NO_NEW_PRIVS must be set before landlock_restrict_self.
	 * This is also done in sandbox_apply_seccomp(), but we need it
	 * here first since landlock comes before seccomp in sandbox_enter().
	 */
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
		int err = errno;
		log_err("sandbox: landlock: PR_SET_NO_NEW_PRIVS failed: %s",
			 strerror(err));
		close(ruleset_fd);
		MORPH_RETURN(-err);
	}

	if (ll_restrict_self(ruleset_fd) < 0) {
		int err = errno;
		log_err("sandbox: landlock: restrict_self failed: %s",
			 strerror(err));
		close(ruleset_fd);
		MORPH_RETURN(-err);
	}

	close(ruleset_fd);
	log_info("sandbox: landlock fs restrictions applied "
		 "(perms=0x%x, %d allowed paths)", permissions, count);
	return 0;
}

#elif defined(__APPLE__)

/*
 * macOS implementation using sandbox_init() with SBPL profile.
 *
 * sandbox_init(profile, 0, &errorbuf) accepts raw SBPL strings
 * when flags=0 (not SANDBOX_NAMED). This is the same approach
 * used by Chromium's Seatbelt wrapper.
 *
 * The SBPL (Sandbox Profile Language) is a Scheme-like DSL that
 * Apple uses internally. Although sandbox_init is marked deprecated,
 * it remains functional on all macOS versions and is the only
 * programmatic way to apply fine-grained filesystem restrictions
 * without wrapping the command in sandbox-exec(1).
 *
 * We must declare sandbox_init/sandbox_free_error directly because
 * our own "sandbox.h" header shadows Apple's system <sandbox.h>.
 */
extern int sandbox_init(const char *profile, uint64_t flags, char **errorbuf);
extern void sandbox_free_error(char *errorbuf);

/* Suppress deprecation warnings — Chromium does the same */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

int sandbox_apply_fs(const char **allowed_paths, int count,
		     unsigned int permissions)
{
	/*
	 * On macOS, filesystem restrictions are handled entirely
	 * by sandbox_enter_darwin() which applies the full SBPL
	 * profile including fs rules. We don't apply fs restrictions
	 * separately here to avoid calling sandbox_init twice.
	 */
	(void)allowed_paths;
	(void)count;
	(void)permissions;
	log_info("sandbox: macOS fs restrictions deferred to "
		 "sandbox_enter_darwin (SBPL profile)");
	return 0;
}

int sandbox_enter_darwin(struct sandbox_config *cfg)
{
	if (!cfg)
		return -EINVAL;

	/* Build SBPL profile string */
	char sbpl[8192];
	int off = 0;
	int n;
	size_t remaining;

	remaining = sizeof(sbpl);
	n = snprintf(sbpl + off, remaining,
		     "(version 1)\n"
		     "(deny default)\n");
	if (n > 0 && (size_t)n < remaining)
		off += n;
	remaining = sizeof(sbpl) - (size_t)off;

	/*
	 * File read: when process execution is allowed, the spawned
	 * process needs to read system libraries (/usr/lib, /System,
	 * etc.) so file-read* must be granted globally.  Otherwise,
	 * restrict to the specified allowed paths only.
	 */
	if (cfg->permissions & EXT_PERM_EXEC) {
		n = snprintf(sbpl + off, remaining,
			     "(allow file-read*)\n");
		if (n > 0 && (size_t)n < remaining)
			off += n;
	} else if (cfg->allowed_paths_count > 0 && cfg->allowed_paths) {
		for (int i = 0; i < cfg->allowed_paths_count; i++) {
			if (!cfg->allowed_paths[i])
				continue;
			remaining = sizeof(sbpl) - (size_t)off;
			n = snprintf(sbpl + off, remaining,
				     "(allow file-read* "
				     "(subpath \"%s\"))\n",
				     cfg->allowed_paths[i]);
			if (n > 0 && (size_t)n < remaining)
				off += n;
		}
	} else {
		n = snprintf(sbpl + off, remaining,
			     "(allow file-read*)\n");
		if (n > 0 && (size_t)n < remaining)
			off += n;
	}
	remaining = sizeof(sbpl) - (size_t)off;

	/* File write: restricted to allowed paths or fully allowed */
	if (cfg->permissions & EXT_PERM_FILESYS) {
		if (cfg->allowed_paths_count > 0 && cfg->allowed_paths) {
			for (int i = 0; i < cfg->allowed_paths_count; i++) {
				if (!cfg->allowed_paths[i])
					continue;
				remaining = sizeof(sbpl) - (size_t)off;
				n = snprintf(sbpl + off, remaining,
					     "(allow file-write* "
					     "(subpath \"%s\"))\n",
					     cfg->allowed_paths[i]);
				if (n > 0 && (size_t)n < remaining)
					off += n;
			}
		} else {
			remaining = sizeof(sbpl) - (size_t)off;
			n = snprintf(sbpl + off, remaining,
				     "(allow file-write*)\n");
			if (n > 0 && (size_t)n < remaining)
				off += n;
		}
	}
	/* No EXT_PERM_FILESYS: writes are denied by (deny default) */

	/* Network access */
	if (cfg->permissions & EXT_PERM_NETWORK) {
		remaining = sizeof(sbpl) - (size_t)off;
		n = snprintf(sbpl + off, remaining,
			     "(allow network*)\n");
		if (n > 0 && (size_t)n < remaining)
			off += n;
		/*
		 * DNS resolution on macOS requires reading
		 * /etc/resolv.conf and accessing mDNSResponder
		 * via Mach IPC (already covered by the
		 * mach-lookup baseline rule).  When EXEC is
		 * not set, file-read* is restricted so we
		 * must add explicit subpath rules for the
		 * resolver paths.
		 */
		if (!(cfg->permissions & EXT_PERM_EXEC)) {
			remaining = sizeof(sbpl) - (size_t)off;
			n = snprintf(sbpl + off, remaining,
				     "(allow file-read* "
				     "(subpath \"/etc/resolv.conf\"))\n"
				     "(allow file-read* "
				     "(subpath \"/private/etc/resolv.conf\"))\n"
				     "(allow file-read* "
				     "(subpath \"/private/var/run/mDNSResponder\"))\n"
				     "(allow file-read* "
				     "(subpath \"/private/var/run/\"))\n");
			if (n > 0 && (size_t)n < remaining)
				off += n;
		}
	}

	/* Process execution */
	if (cfg->permissions & EXT_PERM_EXEC) {
		remaining = sizeof(sbpl) - (size_t)off;
		n = snprintf(sbpl + off, remaining,
			     "(allow process-exec)\n");
		if (n > 0 && (size_t)n < remaining)
			off += n;
	}

	/* Basic process operations needed for normal functioning */
	remaining = sizeof(sbpl) - (size_t)off;
	n = snprintf(sbpl + off, remaining,
		     "(allow process-fork)\n"
		     "(allow signal (target same-sandbox))\n"
		     "(allow mach-lookup)\n"
		     "(allow sysctl-read)\n");
	if (n > 0 && (size_t)n < remaining)
		off += n;

	log_info("sandbox: macOS SBPL profile:\n%s", sbpl);

	char *errorbuf = NULL;
	int rv = sandbox_init(sbpl, 0, &errorbuf);

	if (rv != 0) {
		log_warn("sandbox: sandbox_init failed: %s",
			 errorbuf ? errorbuf : "unknown error");
		if (errorbuf)
			sandbox_free_error(errorbuf);
		return -EPERM;
	}

	if (errorbuf)
		sandbox_free_error(errorbuf);

	log_info("sandbox: macOS sandbox_init applied (perms=0x%x)",
		 cfg->permissions);
	return 0;
}

#pragma clang diagnostic pop

#else /* Neither Linux nor macOS */

int sandbox_apply_fs(const char **allowed_paths, int count,
		     unsigned int permissions)
{
	(void)allowed_paths;
	(void)count;
	(void)permissions;
	log_info("sandbox: filesystem path restrictions not available "
		 "on this platform");
	return 0;
}

#endif /* platform-specific fs implementations */

/* ────────────────────────────────────────────────────────────────
 * Seccomp-BPF (Linux only)
 * ──────────────────────────────────────────────────────────────── */

#ifdef __linux__

#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/prctl.h>
#include <unistd.h>

#if defined(__x86_64__)
#define SECCOMP_ARCH_NR AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define SECCOMP_ARCH_NR AUDIT_ARCH_AARCH64
#elif defined(__arm__)
#define SECCOMP_ARCH_NR AUDIT_ARCH_ARM
#elif defined(__i386__)
#define SECCOMP_ARCH_NR AUDIT_ARCH_I386
#else
#define SECCOMP_ARCH_NR AUDIT_ARCH_X86_64
#endif

static int fb_append(morph_array_t *fb, struct sock_filter insn)
{
	struct sock_filter *slot = morph_array_push(fb);
	if (!slot)
		return -ENOMEM;
	*slot = insn;
	return 0;
}

static int fb_allow(morph_array_t *fb, int nr)
{
	int rc;
	rc = fb_append(fb, (struct sock_filter)BPF_JUMP(
		BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1));
	if (rc < 0)
		return rc;
	rc = fb_append(fb, (struct sock_filter)BPF_STMT(
		BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
	return rc;
}

int sandbox_apply_seccomp(unsigned int permissions)
{
	morph_array_t fb;
	int rc;

	rc = morph_array_init(&fb, 128, sizeof(struct sock_filter));
	if (rc < 0)
		return rc;

	rc = fb_append(&fb, (struct sock_filter)BPF_STMT(
		BPF_LD | BPF_W | BPF_ABS,
		offsetof(struct seccomp_data, arch)));
	if (rc < 0)
		goto fail;
	rc = fb_append(&fb, (struct sock_filter)BPF_JUMP(
		BPF_JMP | BPF_JEQ | BPF_K, SECCOMP_ARCH_NR, 1, 0));
	if (rc < 0)
		goto fail;
	rc = fb_append(&fb, (struct sock_filter)BPF_STMT(
		BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
	if (rc < 0)
		goto fail;

	rc = fb_append(&fb, (struct sock_filter)BPF_STMT(
		BPF_LD | BPF_W | BPF_ABS,
		offsetof(struct seccomp_data, nr)));
	if (rc < 0)
		goto fail;

	rc = fb_allow(&fb, SYS_read);       if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_write);      if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_close);      if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_fstat);      if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_lseek);      if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_mmap);       if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_mprotect);    if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_munmap);      if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_brk);        if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_rt_sigaction); if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_rt_sigprocmask); if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_ioctl);      if (rc < 0) goto fail;
#ifdef SYS_poll
	rc = fb_allow(&fb, SYS_poll);       if (rc < 0) goto fail;
#endif
	rc = fb_allow(&fb, SYS_mremap);     if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_nanosleep);  if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_clock_gettime); if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_getpid);     if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_sendfile);   if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_dup);       if (rc < 0) goto fail;
#ifdef SYS_dup2
	rc = fb_allow(&fb, SYS_dup2);       if (rc < 0) goto fail;
#endif
	rc = fb_allow(&fb, SYS_getdents64); if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_gettid);     if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_futex);      if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_sched_yield); if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_exit_group); if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_exit);       if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_writev);     if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_readv);      if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_set_robust_list); if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_getrandom);  if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_gettimeofday); if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_getrlimit);  if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_ppoll);      if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_pipe2);      if (rc < 0) goto fail;

#ifdef __NR_openat
	rc = fb_allow(&fb, __NR_openat);    if (rc < 0) goto fail;
#endif
#ifdef __NR_faccessat
	rc = fb_allow(&fb, __NR_faccessat);  if (rc < 0) goto fail;
#endif
#ifdef __NR_newfstatat
	rc = fb_allow(&fb, __NR_newfstatat); if (rc < 0) goto fail;
#endif
#ifdef __NR_readlinkat
	rc = fb_allow(&fb, __NR_readlinkat); if (rc < 0) goto fail;
#endif
#ifdef __NR_set_tid_address
	rc = fb_allow(&fb, __NR_set_tid_address); if (rc < 0) goto fail;
#endif
#ifdef __NR_clock_nanosleep
	rc = fb_allow(&fb, __NR_clock_nanosleep); if (rc < 0) goto fail;
#endif
#ifdef __NR_renameat2
	rc = fb_allow(&fb, __NR_renameat2); if (rc < 0) goto fail;
#endif
#ifdef __NR_open
	rc = fb_allow(&fb, __NR_open);      if (rc < 0) goto fail;
#endif
#ifdef __NR_access
	rc = fb_allow(&fb, __NR_access);    if (rc < 0) goto fail;
#endif
#ifdef __NR_stat
	rc = fb_allow(&fb, __NR_stat);      if (rc < 0) goto fail;
#endif
#ifdef __NR_lstat
	rc = fb_allow(&fb, __NR_lstat);     if (rc < 0) goto fail;
#endif

	/*
	 * Additional syscalls needed by most programs (glibc, dynamic
	 * linker, V8/Node.js, Python, Go, etc.).  These are read-only
	 * or process-internal operations that do not compromise sandbox
	 * security.
	 */
#ifdef __NR_fcntl
	rc = fb_allow(&fb, __NR_fcntl);          if (rc < 0) goto fail;
#endif
#ifdef __NR_prlimit64
	rc = fb_allow(&fb, __NR_prlimit64);      if (rc < 0) goto fail;
#endif
#ifdef __NR_rseq
	rc = fb_allow(&fb, __NR_rseq);           if (rc < 0) goto fail;
#endif
#ifdef __NR_getuid
	rc = fb_allow(&fb, __NR_getuid);         if (rc < 0) goto fail;
#endif
#ifdef __NR_getgid
	rc = fb_allow(&fb, __NR_getgid);         if (rc < 0) goto fail;
#endif
#ifdef __NR_geteuid
	rc = fb_allow(&fb, __NR_geteuid);        if (rc < 0) goto fail;
#endif
#ifdef __NR_getegid
	rc = fb_allow(&fb, __NR_getegid);        if (rc < 0) goto fail;
#endif
#ifdef __NR_getppid
	rc = fb_allow(&fb, __NR_getppid);        if (rc < 0) goto fail;
#endif
#ifdef __NR_getcwd
	rc = fb_allow(&fb, __NR_getcwd);         if (rc < 0) goto fail;
#endif
#ifdef __NR_uname
	rc = fb_allow(&fb, __NR_uname);          if (rc < 0) goto fail;
#endif
#ifdef __NR_statx
	rc = fb_allow(&fb, __NR_statx);          if (rc < 0) goto fail;
#endif
#ifdef __NR_statfs
	rc = fb_allow(&fb, __NR_statfs);         if (rc < 0) goto fail;
#endif
#ifdef __NR_fstatfs
	rc = fb_allow(&fb, __NR_fstatfs);        if (rc < 0) goto fail;
#endif
#ifdef __NR_sigaltstack
	rc = fb_allow(&fb, __NR_sigaltstack);    if (rc < 0) goto fail;
#endif
#ifdef __NR_madvise
	rc = fb_allow(&fb, __NR_madvise);        if (rc < 0) goto fail;
#endif
#ifdef __NR_pread64
	rc = fb_allow(&fb, __NR_pread64);        if (rc < 0) goto fail;
#endif
#ifdef __NR_pwrite64
	rc = fb_allow(&fb, __NR_pwrite64);       if (rc < 0) goto fail;
#endif
#ifdef __NR_dup3
	rc = fb_allow(&fb, __NR_dup3);           if (rc < 0) goto fail;
#endif
#ifdef __NR_rt_sigreturn
	rc = fb_allow(&fb, __NR_rt_sigreturn);   if (rc < 0) goto fail;
#endif
#ifdef __NR_sched_getaffinity
	rc = fb_allow(&fb, __NR_sched_getaffinity); if (rc < 0) goto fail;
#endif
#ifdef __NR_sched_getparam
	rc = fb_allow(&fb, __NR_sched_getparam); if (rc < 0) goto fail;
#endif
#ifdef __NR_sched_getscheduler
	rc = fb_allow(&fb, __NR_sched_getscheduler); if (rc < 0) goto fail;
#endif
#ifdef __NR_clock_getres
	rc = fb_allow(&fb, __NR_clock_getres);   if (rc < 0) goto fail;
#endif
#ifdef __NR_memfd_create
	rc = fb_allow(&fb, __NR_memfd_create);   if (rc < 0) goto fail;
#endif
#ifdef __NR_sysinfo
	rc = fb_allow(&fb, __NR_sysinfo);        if (rc < 0) goto fail;
#endif
#ifdef __NR_fadvise64
	rc = fb_allow(&fb, __NR_fadvise64);      if (rc < 0) goto fail;
#endif
#ifdef __NR_inotify_init1
	rc = fb_allow(&fb, __NR_inotify_init1);  if (rc < 0) goto fail;
#endif
#ifdef __NR_inotify_add_watch
	rc = fb_allow(&fb, __NR_inotify_add_watch); if (rc < 0) goto fail;
#endif
#ifdef __NR_getpriority
	rc = fb_allow(&fb, __NR_getpriority);    if (rc < 0) goto fail;
#endif
#ifdef __NR_restart_syscall
	rc = fb_allow(&fb, __NR_restart_syscall); if (rc < 0) goto fail;
#endif
#ifdef __NR_faccessat2
	rc = fb_allow(&fb, __NR_faccessat2);     if (rc < 0) goto fail;
#endif
#ifdef __NR_landlock_create_ruleset
	rc = fb_allow(&fb, __NR_landlock_create_ruleset); if (rc < 0) goto fail;
#endif

	/*
	 * Event-driven I/O syscalls (epoll, eventfd, timerfd).
	 * Needed by libuv, libevent, libev, and any modern runtime
	 * that uses Linux's event loop primitives.
	 */
#ifdef __NR_epoll_create1
	rc = fb_allow(&fb, __NR_epoll_create1);  if (rc < 0) goto fail;
#endif
#ifdef __NR_epoll_ctl
	rc = fb_allow(&fb, __NR_epoll_ctl);      if (rc < 0) goto fail;
#endif
#ifdef __NR_epoll_pwait
	rc = fb_allow(&fb, __NR_epoll_pwait);    if (rc < 0) goto fail;
#endif
#ifdef __NR_epoll_wait
	rc = fb_allow(&fb, __NR_epoll_wait);     if (rc < 0) goto fail;
#endif
#ifdef __NR_eventfd2
	rc = fb_allow(&fb, __NR_eventfd2);       if (rc < 0) goto fail;
#endif
#ifdef __NR_timerfd_create
	rc = fb_allow(&fb, __NR_timerfd_create); if (rc < 0) goto fail;
#endif
#ifdef __NR_timerfd_settime
	rc = fb_allow(&fb, __NR_timerfd_settime); if (rc < 0) goto fail;
#endif
#ifdef __NR_timerfd_gettime
	rc = fb_allow(&fb, __NR_timerfd_gettime); if (rc < 0) goto fail;
#endif
#ifdef __NR_signalfd4
	rc = fb_allow(&fb, __NR_signalfd4);      if (rc < 0) goto fail;
#endif

	/*
	 * io_uring syscalls — Node.js 22+ probes these at startup.
	 * If blocked, Node.js falls back to epoll, but since our
	 * default action is KILL_PROCESS (not ERRNO), a missing
	 * allow entry crashes the process.
	 */
#ifdef __NR_io_uring_setup
	rc = fb_allow(&fb, __NR_io_uring_setup); if (rc < 0) goto fail;
#endif
#ifdef __NR_io_uring_enter
	rc = fb_allow(&fb, __NR_io_uring_enter); if (rc < 0) goto fail;
#endif

	if (permissions & EXT_PERM_NETWORK) {
#ifdef __NR_socket
		rc = fb_allow(&fb, __NR_socket);    if (rc < 0) goto fail;
#endif
#ifdef __NR_connect
		rc = fb_allow(&fb, __NR_connect);  if (rc < 0) goto fail;
#endif
#ifdef __NR_bind
		rc = fb_allow(&fb, __NR_bind);      if (rc < 0) goto fail;
#endif
#ifdef __NR_listen
		rc = fb_allow(&fb, __NR_listen);    if (rc < 0) goto fail;
#endif
#ifdef __NR_accept
		rc = fb_allow(&fb, __NR_accept);    if (rc < 0) goto fail;
#endif
#ifdef __NR_accept4
		rc = fb_allow(&fb, __NR_accept4);   if (rc < 0) goto fail;
#endif
		rc = fb_allow(&fb, SYS_recvfrom);   if (rc < 0) goto fail;
		rc = fb_allow(&fb, SYS_sendto);     if (rc < 0) goto fail;
#ifdef __NR_recvmsg
		rc = fb_allow(&fb, __NR_recvmsg);   if (rc < 0) goto fail;
#endif
#ifdef __NR_sendmsg
		rc = fb_allow(&fb, __NR_sendmsg);   if (rc < 0) goto fail;
#endif
		rc = fb_allow(&fb, SYS_setsockopt); if (rc < 0) goto fail;
		rc = fb_allow(&fb, SYS_getsockopt); if (rc < 0) goto fail;
#ifdef __NR_getsockname
		rc = fb_allow(&fb, __NR_getsockname); if (rc < 0) goto fail;
#endif
#ifdef __NR_getpeername
		rc = fb_allow(&fb, __NR_getpeername); if (rc < 0) goto fail;
#endif
#ifdef __NR_shutdown
		rc = fb_allow(&fb, __NR_shutdown);  if (rc < 0) goto fail;
#endif
#ifdef __NR_sendmmsg
		rc = fb_allow(&fb, __NR_sendmmsg);  if (rc < 0) goto fail;
#endif
#ifdef __NR_recvmmsg
		rc = fb_allow(&fb, __NR_recvmmsg);  if (rc < 0) goto fail;
#endif
	}

	if (permissions & EXT_PERM_EXEC) {
		rc = fb_allow(&fb, SYS_execve);     if (rc < 0) goto fail;
#ifdef __NR_execveat
		rc = fb_allow(&fb, __NR_execveat);  if (rc < 0) goto fail;
#endif
		/*
		 * Thread / process management syscalls needed by
		 * interpreters and runtimes (Node.js, Python, Go, etc.)
		 * that spawn worker threads or child processes.
		 */
#ifdef __NR_clone
		rc = fb_allow(&fb, __NR_clone);     if (rc < 0) goto fail;
#endif
#ifdef __NR_clone3
		rc = fb_allow(&fb, __NR_clone3);    if (rc < 0) goto fail;
#endif
#ifdef __NR_wait4
		rc = fb_allow(&fb, __NR_wait4);     if (rc < 0) goto fail;
#endif
#ifdef __NR_setpgid
		rc = fb_allow(&fb, __NR_setpgid);   if (rc < 0) goto fail;
#endif
#ifdef __NR_capget
		rc = fb_allow(&fb, __NR_capget);    if (rc < 0) goto fail;
#endif
#ifdef __NR_prctl
		rc = fb_allow(&fb, __NR_prctl);     if (rc < 0) goto fail;
#endif
#ifdef __NR_socketpair
		rc = fb_allow(&fb, __NR_socketpair); if (rc < 0) goto fail;
#endif
#ifdef __NR_chdir
		rc = fb_allow(&fb, __NR_chdir);     if (rc < 0) goto fail;
#endif
#ifdef __NR_pidfd_open
		rc = fb_allow(&fb, __NR_pidfd_open); if (rc < 0) goto fail;
#endif
#ifdef __NR_pidfd_send_signal
		rc = fb_allow(&fb, __NR_pidfd_send_signal); if (rc < 0) goto fail;
#endif
#ifdef __NR_tgkill
		rc = fb_allow(&fb, __NR_tgkill);    if (rc < 0) goto fail;
#endif
#ifdef __NR_rt_tgsigqueueinfo
		rc = fb_allow(&fb, __NR_rt_tgsigqueueinfo); if (rc < 0) goto fail;
#endif
#ifdef __NR_getgroups
		rc = fb_allow(&fb, __NR_getgroups);  if (rc < 0) goto fail;
#endif
#ifdef __NR_getresuid
		rc = fb_allow(&fb, __NR_getresuid);  if (rc < 0) goto fail;
#endif
#ifdef __NR_getresgid
		rc = fb_allow(&fb, __NR_getresgid);  if (rc < 0) goto fail;
#endif
#ifdef __NR_setgroups
		rc = fb_allow(&fb, __NR_setgroups);  if (rc < 0) goto fail;
#endif
#ifdef __NR_setsid
		rc = fb_allow(&fb, __NR_setsid);    if (rc < 0) goto fail;
#endif
#ifdef __NR_kill
		rc = fb_allow(&fb, __NR_kill);      if (rc < 0) goto fail;
#endif
#ifdef __NR_capset
		rc = fb_allow(&fb, __NR_capset);    if (rc < 0) goto fail;
#endif
#ifdef __NR_setpriority
		rc = fb_allow(&fb, __NR_setpriority); if (rc < 0) goto fail;
#endif
#ifdef __NR_sched_setaffinity
		rc = fb_allow(&fb, __NR_sched_setaffinity); if (rc < 0) goto fail;
#endif
#ifdef __NR_sched_setscheduler
		rc = fb_allow(&fb, __NR_sched_setscheduler); if (rc < 0) goto fail;
#endif
	}

	if (permissions & EXT_PERM_FILESYS) {
#ifdef __NR_mkdir
		rc = fb_allow(&fb, __NR_mkdir);     if (rc < 0) goto fail;
#endif
#ifdef __NR_mkdirat
		rc = fb_allow(&fb, __NR_mkdirat);   if (rc < 0) goto fail;
#endif
#ifdef __NR_unlink
		rc = fb_allow(&fb, __NR_unlink);    if (rc < 0) goto fail;
#endif
#ifdef __NR_unlinkat
		rc = fb_allow(&fb, __NR_unlinkat);  if (rc < 0) goto fail;
#endif
#ifdef __NR_chmod
		rc = fb_allow(&fb, __NR_chmod);     if (rc < 0) goto fail;
#endif
#ifdef __NR_fchmod
		rc = fb_allow(&fb, __NR_fchmod);    if (rc < 0) goto fail;
#endif
#ifdef __NR_fchmodat
		rc = fb_allow(&fb, __NR_fchmodat);  if (rc < 0) goto fail;
#endif
#ifdef __NR_rename
		rc = fb_allow(&fb, __NR_rename);    if (rc < 0) goto fail;
#endif
#ifdef __NR_renameat
		rc = fb_allow(&fb, __NR_renameat);  if (rc < 0) goto fail;
#endif
#ifdef __NR_rmdir
		rc = fb_allow(&fb, __NR_rmdir);     if (rc < 0) goto fail;
#endif
#ifdef __NR_readlink
		rc = fb_allow(&fb, __NR_readlink);  if (rc < 0) goto fail;
#endif
#ifdef __NR_creat
		rc = fb_allow(&fb, __NR_creat);     if (rc < 0) goto fail;
#endif
#ifdef __NR_truncate
		rc = fb_allow(&fb, __NR_truncate);  if (rc < 0) goto fail;
#endif
#ifdef __NR_ftruncate
		rc = fb_allow(&fb, __NR_ftruncate); if (rc < 0) goto fail;
#endif
#ifdef __NR_fallocate
		rc = fb_allow(&fb, __NR_fallocate); if (rc < 0) goto fail;
#endif
#ifdef __NR_copy_file_range
		rc = fb_allow(&fb, __NR_copy_file_range); if (rc < 0) goto fail;
#endif
#ifdef __NR_fdatasync
		rc = fb_allow(&fb, __NR_fdatasync);  if (rc < 0) goto fail;
#endif
#ifdef __NR_flock
		rc = fb_allow(&fb, __NR_flock);      if (rc < 0) goto fail;
#endif
#ifdef __NR_symlinkat
		rc = fb_allow(&fb, __NR_symlinkat);  if (rc < 0) goto fail;
#endif
	}

	/*
	 * Default action for unmatched syscalls.
	 *
	 * Without EXEC permission: KILL_PROCESS — strict sandbox,
	 * no subprocess execution expected.
	 *
	 * With EXEC permission: ERRNO(ENOSYS) — subprocess-heavy
	 * extensions (e.g., Chromium) may use syscalls we haven't
	 * explicitly allowed.  Returning ENOSYS lets them degrade
	 * gracefully instead of being killed, and allows their own
	 * seccomp filters to stack on top of ours without conflict.
	 */
	unsigned int default_action;
	if (permissions & EXT_PERM_EXEC)
		default_action = SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA);
	else
		default_action = SECCOMP_RET_KILL_PROCESS;

	rc = fb_append(&fb, (struct sock_filter)BPF_STMT(
		BPF_RET | BPF_K, default_action));
	if (rc < 0)
		goto fail;

	/*
	 * PR_SET_NO_NEW_PRIVS may have already been set by
	 * sandbox_apply_fs (landlock). Setting it again is harmless.
	 */
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
		int err = errno;
		log_err("sandbox: PR_SET_NO_NEW_PRIVS failed: %s",
			strerror(err));
		rc = -err;
		goto fail;
	}

	struct sock_fprog prog;
	prog.len = (unsigned short)fb.nelts;
	prog.filter = fb.elts;

	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0) {
		int err = errno;
		log_err("sandbox: SECCOMP_MODE_FILTER failed: %s",
			strerror(err));
		rc = -err;
		goto fail;
	}

	log_info("sandbox: seccomp-bpf filter installed (%zu instructions, "
		 "perms=0x%x)", fb.nelts, permissions);
	morph_array_cleanup(&fb);
	return 0;

fail:
	morph_array_cleanup(&fb);
	return rc;
}

#endif /* __linux__ */

#if !defined(__linux__)

int sandbox_apply_seccomp(unsigned int permissions)
{
	(void)permissions;
	log_info("sandbox: seccomp-bpf not available on this platform, "
		 "using rlimits only");
	return 0;
}

#endif /* !__linux__ */

int sandbox_enter(struct sandbox_config *cfg)
{
	if (!cfg)
		return -EINVAL;

	log_info("sandbox_enter: perms=0x%x mem=%dMB cpu=%ds fsize=%dMB nproc=%d",
		 cfg->permissions, cfg->max_memory_mb, cfg->max_cpu_seconds,
		 cfg->max_file_size_mb, cfg->max_processes);

	int rc;

	rc = sandbox_apply_env((const char **)cfg->allowed_env,
			       cfg->allowed_env_count,
			       cfg->permissions);
	if (rc < 0)
		return rc;

	rc = sandbox_apply_rlimits(cfg->permissions, cfg->max_memory_mb,
				   cfg->max_cpu_seconds,
				   cfg->max_file_size_mb,
				   cfg->max_processes,
				   cfg->max_open_files);
	if (rc < 0)
		return rc;

	rc = sandbox_apply_fs((const char **)cfg->allowed_paths,
			       cfg->allowed_paths_count,
			       cfg->permissions);
	if (rc < 0)
		return rc;

#ifdef __linux__
	rc = sandbox_apply_seccomp(cfg->permissions);
	if (rc < 0)
		return rc;
#elif defined(__APPLE__)
	rc = sandbox_enter_darwin(cfg);
	if (rc < 0)
		return rc;
#else
	log_err("sandbox: no platform-specific isolation available");
	return -ENOSYS;
#endif

	return 0;
}
