#include "sandbox.h"
#include "util/array.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/log.h"
#include <errno.h>
#include <limits.h>
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

	rl.rlim_cur = max_open_files > 0 ? (rlim_t)max_open_files : 1024;
	rl.rlim_max = max_open_files > 0 ? (rlim_t)max_open_files : 1024;
	if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
		log_warn("sandbox: setrlimit RLIMIT_NOFILE failed: %s",
			 strerror(errno));
	} else {
		log_info("sandbox: RLIMIT_NOFILE set to %d",
			 max_open_files > 0 ? max_open_files : 1024);
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
 * Fails closed if the kernel does not support Landlock. Seccomp and
 * rlimits alone cannot enforce the declared filesystem paths.
 *
 * Requires kernel headers >= 5.13 for <linux/landlock.h>.
 * Ubuntu 20.04 and older are NOT supported — use Ubuntu >= 22.04
 * or equivalent distro.
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <linux/landlock.h>

/* LANDLOCK_ACCESS_FS_TRUNCATE was added in Landlock ABI v3 (kernel 6.2).
 * Older kernel headers (e.g. Debian 12 / linux-libc-dev 6.1) lack it. */
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE       (1ULL << 14)
#endif

#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV       (1ULL << 15)
#endif

#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION 1
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

static int ll_get_abi(void)
{
	return (int)syscall(__NR_landlock_create_ruleset, NULL, 0,
			    LANDLOCK_CREATE_RULESET_VERSION);
}

static int ll_add_rule(int ruleset_fd, int path_fd, uint64_t access)
{
	struct landlock_path_beneath_attr pb;
	struct stat st;

	/*
	 * Landlock rejects directory-only rights (READ_DIR, MAKE_*,
	 * REMOVE_*) when the rule target is not a directory.  Mask them
	 * out so callers can pass a uniform access mask for both files
	 * and directories.
	 */
	if (fstat(path_fd, &st) == 0 && !S_ISDIR(st.st_mode))
		access &= LANDLOCK_ACCESS_FS_READ_FILE |
			  LANDLOCK_ACCESS_FS_WRITE_FILE |
			  LANDLOCK_ACCESS_FS_EXECUTE |
			  LANDLOCK_ACCESS_FS_TRUNCATE |
			  LANDLOCK_ACCESS_FS_IOCTL_DEV;
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

static int sandbox_add_landlock_pty(int ruleset_fd, uint64_t access)
{
	static const char *const paths[] = { "/dev/ptmx", "/dev/pts", NULL };

	for (int i = 0; paths[i]; i++) {
		int fd = open(paths[i], O_PATH | O_CLOEXEC);

		if (fd < 0) {
			if (errno == ENOENT)
				continue;
			MORPH_RETURN_ERRNO();
		}
		if (ll_add_rule(ruleset_fd, fd, access) < 0) {
			int err = errno;

			close(fd);
			MORPH_RETURN(-err);
		}
		close(fd);
	}
	return 0;
}

static int sandbox_add_landlock_temp(int ruleset_fd, uint64_t access)
{
	static const char *const paths[] = { "/tmp", "/var/tmp", NULL };

	for (int i = 0; paths[i]; i++) {
		int fd = open(paths[i], O_PATH | O_CLOEXEC);

		if (fd < 0) {
			if (errno == ENOENT)
				continue;
			MORPH_RETURN_ERRNO();
		}
		if (ll_add_rule(ruleset_fd, fd, access) < 0) {
			int err = errno;

			close(fd);
			MORPH_RETURN(-err);
		}
		close(fd);
	}
	return 0;
}

static int sandbox_add_landlock_ipc(int ruleset_fd, uint64_t access)
{
	int fd = open("/dev/shm", O_PATH | O_CLOEXEC);

	if (fd < 0) {
		if (errno == ENOENT)
			return 0;
		MORPH_RETURN_ERRNO();
	}
	if (ll_add_rule(ruleset_fd, fd, access) < 0) {
		int err = errno;

		close(fd);
		MORPH_RETURN(-err);
	}
	close(fd);
	return 0;
}

static int sandbox_add_landlock_runtime_proc(int ruleset_fd, uint64_t access,
					      int allow_process_info)
{
	static const char *const process_paths[] = { "/proc", NULL };
	static const char *const runtime_paths[] = {
		"/proc/self", "/proc/thread-self", "/proc/cpuinfo",
		"/proc/meminfo", "/proc/stat", "/proc/filesystems",
		"/proc/mounts", "/proc/sys/kernel/hostname",
		"/proc/sys/kernel/osrelease", NULL
	};
	const char *const *paths = allow_process_info ? process_paths :
		runtime_paths;

	for (int i = 0; paths[i]; i++) {
		int fd = open(paths[i], O_PATH | O_CLOEXEC);

		if (fd < 0) {
			if (errno == ENOENT)
				continue;
			MORPH_RETURN_ERRNO();
		}
		if (ll_add_rule(ruleset_fd, fd, access) < 0) {
			int err = errno;

			close(fd);
			MORPH_RETURN(-err);
		}
		close(fd);
	}
	return 0;
}

int sandbox_apply_fs(const char **allowed_paths, int count,
		     unsigned int permissions)
{
	int abi;
	/*
	 * Probe for landlock support.
	 * Landlock ABI v4+ (kernel 6.6+) rejects rulesets with
	 * handled_access_fs=0, returning ENOMSG.  Use a non-zero
	 * access mask so the probe works across all ABI versions.
	 */
	abi = ll_get_abi();
	if (abi < 0) {
		if (errno == EOPNOTSUPP || errno == ENOSYS) {
			log_err("sandbox: landlock not supported by kernel");
			return -ENOSYS;
		}
		int err = errno;
		log_err("sandbox: landlock probe failed: %s",
			strerror(err));
		MORPH_RETURN(-err);
	}

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
	if (abi < 2)
		write_access &= ~LANDLOCK_ACCESS_FS_REFER;
	if (abi < 3)
		write_access &= ~LANDLOCK_ACCESS_FS_TRUNCATE;

	uint64_t handled = read_access | write_access;
	int needs_write_rules = 0;

	if (abi >= 5)
		handled |= LANDLOCK_ACCESS_FS_IOCTL_DEV;

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
	if (permissions & EXT_PERM_PTY) {
		uint64_t pty_access = handled &
			(LANDLOCK_ACCESS_FS_READ_FILE |
			 LANDLOCK_ACCESS_FS_WRITE_FILE);
		int pty_rc;

		if (abi >= 5)
			pty_access |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
		pty_rc = sandbox_add_landlock_pty(ruleset_fd, pty_access);
		if (pty_rc != 0) {
			close(ruleset_fd);
			return pty_rc;
		}
		if (abi < 5)
			log_warn("sandbox: Landlock ABI %d cannot scope PTY ioctl",
				 abi);
	}
	if (permissions & EXT_PERM_TEMP) {
		uint64_t temp_access = handled &
			(write_access | read_access);
		int temp_rc = sandbox_add_landlock_temp(ruleset_fd,
			temp_access);

		if (temp_rc != 0) {
			close(ruleset_fd);
			return temp_rc;
		}
	}
	if (permissions & EXT_PERM_IPC) {
		uint64_t ipc_access = handled &
			(write_access | read_access);
		int ipc_rc = sandbox_add_landlock_ipc(ruleset_fd, ipc_access);

		if (ipc_rc != 0) {
			close(ruleset_fd);
			return ipc_rc;
		}
	}
	if (handled & write_access) {
		int fd = open("/dev/null", O_PATH | O_CLOEXEC);
		uint64_t dev_null_access = LANDLOCK_ACCESS_FS_WRITE_FILE |
			(handled & LANDLOCK_ACCESS_FS_TRUNCATE);

		if (fd < 0) {
			int err = errno;

			close(ruleset_fd);
			MORPH_RETURN(-err);
		}
		if (ll_add_rule(ruleset_fd, fd, dev_null_access) < 0) {
			int err = errno;

			close(fd);
			close(ruleset_fd);
			MORPH_RETURN(-err);
		}
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
			"/sys",
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
		int proc_rc = sandbox_add_landlock_runtime_proc(ruleset_fd,
			read_access,
			!!(permissions & EXT_PERM_PROCESS_INFO));

		if (proc_rc != 0) {
			close(ruleset_fd);
			return proc_rc;
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

static int sandbox_add_landlock_paths(int ruleset_fd, char **paths,
				      int count, uint64_t access)
{
	for (int i = 0; i < count; i++) {
		int fd;

		if (!paths || !paths[i])
			continue;
		fd = open(paths[i], O_PATH | O_CLOEXEC);
		if (fd < 0) {
			/*
			 * Optional policy paths may legitimately not
			 * exist on this host; skipping one must not
			 * disable the whole sandbox.
			 */
			log_warn("sandbox: skipping policy path '%s': %s",
				 paths[i], strerror(errno));
			continue;
		}
		if (ll_add_rule(ruleset_fd, fd, access) < 0) {
			log_warn("sandbox: cannot add policy path '%s': %s",
				 paths[i], strerror(errno));
			close(fd);
			continue;
		}
		close(fd);
	}
	return 0;
}

static void sandbox_try_writable_path(int ruleset_fd, uint64_t access,
				      const char *path)
{
	int fd;

	if (!path || !*path)
		return;
	fd = open(path, O_PATH | O_CLOEXEC);
	if (fd < 0)
		return;
	if (ll_add_rule(ruleset_fd, fd, access) < 0)
		log_warn("sandbox: default writable path '%s' rejected: %s",
			 path, strerror(errno));
	close(fd);
}

static void sandbox_add_xdg_dir(int ruleset_fd, uint64_t access,
				const char *var, const char *fallback)
{
	const char *value = getenv(var);
	const char *home;
	char path[PATH_MAX];
	int written;

	if (value && *value) {
		sandbox_try_writable_path(ruleset_fd, access, value);
		return;
	}
	home = getenv("HOME");
	if (!home || !*home)
		return;
	written = snprintf(path, sizeof(path), "%s/%s", home, fallback);
	if (written > 0 && (size_t)written < sizeof(path))
		sandbox_try_writable_path(ruleset_fd, access, path);
}

static void sandbox_add_default_writable_dirs(int ruleset_fd,
					      uint64_t access)
{
	static const char *const home_dirs[] = {
		".cargo", ".rustup", ".npm", ".gradle", ".m2", ".go",
		"go", ".local/bin", NULL
	};
	const char *home = getenv("HOME");
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char path[PATH_MAX];

	sandbox_add_xdg_dir(ruleset_fd, access, "XDG_CACHE_HOME", ".cache");
	sandbox_add_xdg_dir(ruleset_fd, access, "XDG_CONFIG_HOME", ".config");
	sandbox_add_xdg_dir(ruleset_fd, access, "XDG_DATA_HOME",
			    ".local/share");
	sandbox_add_xdg_dir(ruleset_fd, access, "XDG_STATE_HOME",
			    ".local/state");
	if (runtime && *runtime)
		sandbox_try_writable_path(ruleset_fd, access, runtime);
	sandbox_try_writable_path(ruleset_fd, access, "/var/tmp");
	sandbox_try_writable_path(ruleset_fd, access, "/dev/tty");
	if (!home || !*home)
		return;
	for (int i = 0; home_dirs[i]; i++) {
		int written = snprintf(path, sizeof(path), "%s/%s", home,
				       home_dirs[i]);

		if (written > 0 && (size_t)written < sizeof(path))
			sandbox_try_writable_path(ruleset_fd, access, path);
	}
}

static int sandbox_apply_path_policy(struct sandbox_config *cfg)
{
	static const char *const system_paths[] = {
		"/bin", "/sbin", "/usr", "/lib", "/lib32", "/lib64",
		"/etc", "/dev", "/sys", "/run", "/snap",
		"/opt", "/nix", NULL
	};
	uint64_t read_access = LANDLOCK_ACCESS_FS_READ_FILE |
		LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE;
	uint64_t write_access = LANDLOCK_ACCESS_FS_WRITE_FILE |
		LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_DIR |
		LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_MAKE_CHAR |
		LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
		LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_TRUNCATE;
	uint64_t delete_access = LANDLOCK_ACCESS_FS_REMOVE_FILE |
		LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REFER;
	uint64_t handled;
	int abi;
	int ruleset_fd;
	int rc;

	if (!cfg)
		MORPH_RETURN(-EINVAL);
	abi = ll_get_abi();
	if (abi < 0) {
		if (errno == EOPNOTSUPP || errno == ENOSYS)
			MORPH_RETURN(-ENOSYS);
		MORPH_RETURN_ERRNO();
	}
	if (abi < 2)
		delete_access &= ~LANDLOCK_ACCESS_FS_REFER;
	if (abi < 3)
		write_access &= ~LANDLOCK_ACCESS_FS_TRUNCATE;
	handled = write_access | delete_access;
	if (!cfg->read_all)
		handled |= read_access;
	if (abi >= 5)
		handled |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
	ruleset_fd = ll_create_ruleset(handled);
	if (ruleset_fd < 0)
		MORPH_RETURN_ERRNO();
	if (!cfg->read_all) {
		rc = sandbox_add_landlock_paths(ruleset_fd, cfg->read_paths,
			cfg->read_paths_count, read_access);
		if (rc != 0) {
			close(ruleset_fd);
			return rc;
		}
		if (cfg->process_exec) {
			for (int i = 0; system_paths[i]; i++) {
				char *path = (char *)system_paths[i];

				if (access(path, F_OK) != 0)
					continue;
				rc = sandbox_add_landlock_paths(ruleset_fd,
					&path, 1, read_access);
				if (rc != 0) {
					close(ruleset_fd);
					return rc;
				}
			}
			rc = sandbox_add_landlock_runtime_proc(ruleset_fd,
				read_access, cfg->allow_process_info);
			if (rc != 0) {
				close(ruleset_fd);
				return rc;
			}
		}
	}
	rc = sandbox_add_landlock_paths(ruleset_fd, cfg->write_paths,
		cfg->write_paths_count, write_access);
	if (rc == 0)
		sandbox_add_default_writable_dirs(ruleset_fd, write_access);
	if (rc == 0) {
		char *dev_null = (char *)"/dev/null";
		uint64_t dev_null_access = LANDLOCK_ACCESS_FS_WRITE_FILE |
			(write_access & LANDLOCK_ACCESS_FS_TRUNCATE);

		rc = sandbox_add_landlock_paths(ruleset_fd, &dev_null, 1,
			dev_null_access);
	}
	if (rc == 0)
		rc = sandbox_add_landlock_paths(ruleset_fd, cfg->delete_paths,
			cfg->delete_paths_count, delete_access);
	if (rc == 0 && cfg->allow_pty) {
		uint64_t pty_access = LANDLOCK_ACCESS_FS_WRITE_FILE |
			(handled & LANDLOCK_ACCESS_FS_READ_FILE);

		if (abi >= 5)
			pty_access |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
		rc = sandbox_add_landlock_pty(ruleset_fd, pty_access);
		if (rc == 0 && abi < 5)
			log_warn("sandbox: Landlock ABI %d cannot scope PTY ioctl",
				 abi);
	}
	if (rc == 0 && cfg->allow_temp) {
		uint64_t temp_access = write_access | delete_access |
			(handled & read_access);

		rc = sandbox_add_landlock_temp(ruleset_fd, temp_access);
	}
	if (rc == 0 && cfg->allow_ipc) {
		uint64_t ipc_access = write_access | delete_access |
			(handled & read_access);

		rc = sandbox_add_landlock_ipc(ruleset_fd, ipc_access);
	}
	if (rc != 0) {
		close(ruleset_fd);
		return rc;
	}
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
		int err = errno;

		close(ruleset_fd);
		MORPH_RETURN(-err);
	}
	if (ll_restrict_self(ruleset_fd) < 0) {
		int err = errno;

		close(ruleset_fd);
		MORPH_RETURN(-err);
	}
	close(ruleset_fd);
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

static int sandbox_sbpl_path_rule(morph_buf_t *profile, const char *operations,
				  const char *path)
{
	int rc;

	if (!profile || !operations || !path)
		MORPH_RETURN(-EINVAL);
	rc = morph_buf_printf(profile, "(allow %s (subpath \"", operations);
	if (rc != 0)
		return rc;
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		if (*p < 0x20 || *p == 0x7f)
			MORPH_RETURN(-EINVAL);
		if (*p == '\\' || *p == '"')
			rc = morph_buf_putc(profile, '\\');
		if (rc == 0)
			rc = morph_buf_putc(profile, (char)*p);
		if (rc != 0)
			return rc;
	}
	return morph_buf_puts(profile, "\"))\n");
}

static int sandbox_sbpl_darwin_user_dir(morph_buf_t *profile, int name)
{
	char path[PATH_MAX];
	char resolved[PATH_MAX];
	size_t length;
	int rc;

	length = confstr(name, path, sizeof(path));
	if (length == 0)
		return 0;
	if (length > sizeof(path))
		MORPH_RETURN(-ENAMETOOLONG);
	if (!realpath(path, resolved))
		MORPH_RETURN_ERRNO();
	rc = sandbox_sbpl_path_rule(profile, "file-read*", resolved);
	if (rc != 0)
		return rc;
	return sandbox_sbpl_path_rule(profile,
		"file-write-data file-write-create file-write-mode "
		"file-write-flags file-write-owner file-write-times "
		"file-write-xattr file-write-unlink", resolved);
}

static int sandbox_sbpl_base(morph_buf_t *profile)
{
	return morph_buf_puts(profile,
		"(version 1)\n"
		"(deny default)\n"
		"(allow file-read-metadata)\n");
}

/*
 * Generic macOS runtime policy.
 *
 * Keep the runtime layer deliberately application-agnostic.  A code agent
 * must be able to launch ordinary CLI and GUI applications without carrying
 * a growing per-application list of Mach/XPC services.
 *
 * The actual security boundary remains filesystem data/write access and
 * network access, which are emitted separately from this runtime policy.
 */
static int sandbox_sbpl_runtime(morph_buf_t *profile)
{
	static const char *const runtime_read_paths[] = {
		"/Applications",
		"/bin",
		"/sbin",
		"/usr",
		"/System",
		"/Library",
		"/private/etc",
		"/private/var/db/timezone",
		"/private/var/select",
		"/opt",
		"/nix",
		NULL
	};
	int rc;

	rc = morph_buf_puts(profile,
		"(allow process-exec)\n"
		"(allow process-fork)\n"
		"(allow signal (target same-sandbox))\n"
		"(allow process-info*)\n"
		"(allow mach-lookup)\n"
		"(allow mach-register)\n"
		"(allow ipc-posix-sem)\n"
		"(allow ipc-posix-shm-read* ipc-posix-shm-write*)\n"
		"(allow sysctl-read)\n"
		"(allow pseudo-tty)\n"
		"(allow file-read* file-write* file-ioctl "
		"(literal \"/dev/null\"))\n"
		"(allow file-read* file-write* file-ioctl "
		"(literal \"/dev/zero\"))\n"
		"(allow file-read* (literal \"/dev/random\"))\n"
		"(allow file-read* (literal \"/dev/urandom\"))\n"
		"(allow file-read* file-write* file-ioctl "
		"(literal \"/dev/ptmx\"))\n"
		"(allow file-read* file-write*\n"
		"  (require-all (regex #\"^/dev/ttys[0-9]+\")\n"
		"    (extension \"com.apple.sandbox.pty\")))\n"
		"(allow file-ioctl (regex #\"^/dev/ttys[0-9]+\"))\n"
		"(allow iokit-open\n"
		"  (iokit-registry-entry-class \"RootDomainUserClient\"))\n");
	if (rc != 0)
		return rc;

	for (int i = 0; runtime_read_paths[i]; i++) {
		rc = sandbox_sbpl_path_rule(profile, "file-read*",
			runtime_read_paths[i]);
		if (rc != 0)
			return rc;
	}

	return 0;
}

static int sandbox_sbpl_temp(morph_buf_t *profile)
{
	int rc;

	rc = sandbox_sbpl_darwin_user_dir(profile, _CS_DARWIN_USER_TEMP_DIR);
	if (rc != 0)
		return rc;

	return sandbox_sbpl_path_rule(profile,
		"file-read* file-write-data file-write-create "
		"file-write-mode file-write-flags file-write-owner "
		"file-write-times file-write-xattr file-write-unlink",
		"/private/tmp");
}

static int sandbox_sbpl_filesystem(morph_buf_t *profile,
				   const struct sandbox_config *cfg)
{
	char **paths;
	int count;
	int rc;

	/*
	 * Let runtimes traverse/probe the root without granting arbitrary file
	 * contents.  The actual readable subtrees are emitted below.
	 */
	rc = morph_buf_puts(profile,
		"(allow file-read-data (literal \"/\"))\n"
		"(allow file-write-data (literal \"/dev/null\"))\n");
	if (rc != 0)
		return rc;

	if (cfg->path_policy_enabled && cfg->read_all) {
		rc = morph_buf_puts(profile, "(allow file-read*)\n");
		if (rc != 0)
			return rc;
	} else {
		paths = cfg->path_policy_enabled ? cfg->read_paths :
			cfg->allowed_paths;
		count = cfg->path_policy_enabled ? cfg->read_paths_count :
			cfg->allowed_paths_count;

		for (int i = 0; i < count; i++) {
			if (!paths || !paths[i])
				continue;
			rc = sandbox_sbpl_path_rule(profile, "file-read*", paths[i]);
			if (rc != 0)
				return rc;
		}
	}

	if ((cfg->path_policy_enabled && cfg->allow_temp) ||
	    (!cfg->path_policy_enabled &&
	     (cfg->permissions & EXT_PERM_TEMP))) {
		rc = sandbox_sbpl_temp(profile);
		if (rc != 0)
			return rc;
	}

	if (cfg->path_policy_enabled) {
		for (int i = 0; i < cfg->write_paths_count; i++) {
			if (!cfg->write_paths || !cfg->write_paths[i])
				continue;
			rc = sandbox_sbpl_path_rule(profile,
				"file-write-data file-write-create "
				"file-write-mode file-write-flags "
				"file-write-owner file-write-times "
				"file-write-xattr",
				cfg->write_paths[i]);
			if (rc != 0)
				return rc;
		}

		for (int i = 0; i < cfg->delete_paths_count; i++) {
			if (!cfg->delete_paths || !cfg->delete_paths[i])
				continue;
			rc = sandbox_sbpl_path_rule(profile, "file-write-unlink",
				cfg->delete_paths[i]);
			if (rc != 0)
				return rc;
		}
		return 0;
	}

	if (!(cfg->permissions & EXT_PERM_FILESYS))
		return 0;

	if (cfg->allowed_paths_count > 0 && cfg->allowed_paths) {
		for (int i = 0; i < cfg->allowed_paths_count; i++) {
			if (!cfg->allowed_paths[i])
				continue;
			rc = sandbox_sbpl_path_rule(profile, "file-write*",
				cfg->allowed_paths[i]);
			if (rc != 0)
				return rc;
		}
		return 0;
	}

	return morph_buf_puts(profile, "(allow file-write*)\n");
}

static int sandbox_sbpl_network(morph_buf_t *profile,
				const struct sandbox_config *cfg)
{
	int network_access;

	network_access = cfg->path_policy_enabled ? cfg->network_access :
		!!(cfg->permissions & EXT_PERM_NETWORK);
	if (!network_access)
		return 0;

	return morph_buf_puts(profile, "(allow network*)\n");
}

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
	morph_buf_t sbpl;
	char *profile;
	char *errorbuf = NULL;
	int rc;
	int rv;

	if (!cfg)
		return -EINVAL;

	rc = morph_buf_init(&sbpl, 4096);
	if (rc != 0)
		return rc;

	rc = sandbox_sbpl_base(&sbpl);
	if (rc == 0)
		rc = sandbox_sbpl_runtime(&sbpl);
	if (rc == 0)
		rc = sandbox_sbpl_filesystem(&sbpl, cfg);
	if (rc == 0)
		rc = sandbox_sbpl_network(&sbpl, cfg);

	if (rc != 0 || sbpl.failed) {
		morph_buf_cleanup(&sbpl);
		return rc != 0 ? rc : -ENOMEM;
	}

	profile = morph_buf_detach(&sbpl);
	if (!profile)
		return -ENOMEM;

	log_info("sandbox: macOS SBPL profile:\n%s", profile);
	rv = sandbox_init(profile, 0, &errorbuf);
	free(profile);

	if (rv != 0) {
		log_warn("sandbox: sandbox_init failed: %s",
			 errorbuf ? errorbuf : "unknown error");
		if (errorbuf)
			sandbox_free_error(errorbuf);
		return -EPERM;
	}

	if (errorbuf)
		sandbox_free_error(errorbuf);

	log_info("sandbox: macOS sandbox_init applied");
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
#include <sys/socket.h>
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

/*
 * Seccomp-BPF denylist.
 *
 * The default action is ALLOW on purpose.  A syscall allowlist cannot
 * keep pace with the kernel and breaks runtimes (Node.js, Python, Go,
 * glibc) whenever they issue a syscall that is not listed.  We only
 * deny the operations that would let the sandbox escape, and return
 * EPERM so callers degrade gracefully instead of being killed.
 */

#define SECCOMP_DENY(fbp, nr) \
	do { \
		rc = fb_deny_syscall((fbp), (nr)); \
		if (rc < 0) \
			goto fail; \
	} while (0)

#define SECCOMP_DENY_IP_SOCKET(fbp, nr) \
	do { \
		rc = fb_deny_ip_socket((fbp), (nr)); \
		if (rc < 0) \
			goto fail; \
	} while (0)

static int fb_stmt(morph_array_t *fb, unsigned int code, unsigned int k)
{
	return fb_append(fb, (struct sock_filter)BPF_STMT(code, k));
}

static int fb_jump(morph_array_t *fb, unsigned int code, unsigned int k,
		   unsigned char jt, unsigned char jf)
{
	return fb_append(fb, (struct sock_filter)BPF_JUMP(code, k, jt, jf));
}

static int fb_ret(morph_array_t *fb, unsigned int action)
{
	return fb_stmt(fb, BPF_RET | BPF_K, action);
}

static int fb_deny_syscall(morph_array_t *fb, int nr)
{
	int rc;

	if (nr < 0)
		return 0;
	rc = fb_jump(fb, BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)nr, 0, 1);
	if (rc < 0)
		return rc;
	return fb_ret(fb, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
}

/*
 * Deny a socket-family syscall unless arg0 is AF_UNIX.  Emits a
 * self-contained block that reloads the syscall number afterwards so
 * the linear nr dispatch can continue.
 */
static int fb_deny_ip_socket(morph_array_t *fb, int nr)
{
	int rc;

	if (nr < 0)
		return 0;
	rc = fb_jump(fb, BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)nr, 0, 4);
	if (rc < 0)
		return rc;
	rc = fb_stmt(fb, BPF_LD | BPF_W | BPF_ABS,
		(unsigned int)offsetof(struct seccomp_data, args[0]));
	if (rc < 0)
		return rc;
	rc = fb_jump(fb, BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)AF_UNIX,
		1, 0);
	if (rc < 0)
		return rc;
	rc = fb_ret(fb, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
	if (rc < 0)
		return rc;
	return fb_stmt(fb, BPF_LD | BPF_W | BPF_ABS,
		(unsigned int)offsetof(struct seccomp_data, nr));
}

int sandbox_apply_seccomp(unsigned int permissions)
{
	morph_array_t fb;
	int rc;

	rc = morph_array_init(&fb, 128, sizeof(struct sock_filter));
	if (rc < 0)
		return rc;

	rc = fb_stmt(&fb, BPF_LD | BPF_W | BPF_ABS,
		(unsigned int)offsetof(struct seccomp_data, arch));
	if (rc < 0)
		goto fail;
	rc = fb_jump(&fb, BPF_JMP | BPF_JEQ | BPF_K, SECCOMP_ARCH_NR, 1, 0);
	if (rc < 0)
		goto fail;
	rc = fb_ret(&fb, SECCOMP_RET_KILL_PROCESS);
	if (rc < 0)
		goto fail;
	rc = fb_stmt(&fb, BPF_LD | BPF_W | BPF_ABS,
		(unsigned int)offsetof(struct seccomp_data, nr));
	if (rc < 0)
		goto fail;

	/*
	 * Process introspection: ptrace and friends can read or rewrite
	 * another same-uid process (including the agent itself), which
	 * landlock does not mediate.  Always deny them; EXT_PERM_PROCESS_INFO
	 * only widens /proc visibility.
	 */
#ifdef SYS_ptrace
	SECCOMP_DENY(&fb, SYS_ptrace);
#endif
#ifdef SYS_process_vm_readv
	SECCOMP_DENY(&fb, SYS_process_vm_readv);
#endif
#ifdef SYS_process_vm_writev
	SECCOMP_DENY(&fb, SYS_process_vm_writev);
#endif
#ifdef SYS_pidfd_getfd
	SECCOMP_DENY(&fb, SYS_pidfd_getfd);
#endif

	/*
	 * Host-level operations that need capabilities we never grant.
	 * EPERM matches what an unprivileged caller would observe.
	 */
#ifdef SYS_mount
	SECCOMP_DENY(&fb, SYS_mount);
#endif
#ifdef SYS_umount2
	SECCOMP_DENY(&fb, SYS_umount2);
#endif
#ifdef SYS_pivot_root
	SECCOMP_DENY(&fb, SYS_pivot_root);
#endif
#ifdef SYS_chroot
	SECCOMP_DENY(&fb, SYS_chroot);
#endif
#ifdef SYS_kexec_load
	SECCOMP_DENY(&fb, SYS_kexec_load);
#endif
#ifdef SYS_kexec_file_load
	SECCOMP_DENY(&fb, SYS_kexec_file_load);
#endif
#ifdef SYS_reboot
	SECCOMP_DENY(&fb, SYS_reboot);
#endif
#ifdef SYS_swapon
	SECCOMP_DENY(&fb, SYS_swapon);
#endif
#ifdef SYS_swapoff
	SECCOMP_DENY(&fb, SYS_swapoff);
#endif
#ifdef SYS_init_module
	SECCOMP_DENY(&fb, SYS_init_module);
#endif
#ifdef SYS_finit_module
	SECCOMP_DENY(&fb, SYS_finit_module);
#endif
#ifdef SYS_delete_module
	SECCOMP_DENY(&fb, SYS_delete_module);
#endif
#ifdef SYS_bpf
	SECCOMP_DENY(&fb, SYS_bpf);
#endif
#ifdef SYS_userfaultfd
	SECCOMP_DENY(&fb, SYS_userfaultfd);
#endif
#ifdef SYS_perf_event_open
	SECCOMP_DENY(&fb, SYS_perf_event_open);
#endif
#ifdef SYS_keyctl
	SECCOMP_DENY(&fb, SYS_keyctl);
#endif
#ifdef SYS_add_key
	SECCOMP_DENY(&fb, SYS_add_key);
#endif
#ifdef SYS_request_key
	SECCOMP_DENY(&fb, SYS_request_key);
#endif
#ifdef SYS_acct
	SECCOMP_DENY(&fb, SYS_acct);
#endif
#ifdef SYS_quotactl
	SECCOMP_DENY(&fb, SYS_quotactl);
#endif
#ifdef SYS_open_by_handle_at
	SECCOMP_DENY(&fb, SYS_open_by_handle_at);
#endif
#ifdef SYS_name_to_handle_at
	SECCOMP_DENY(&fb, SYS_name_to_handle_at);
#endif
#ifdef SYS_io_uring_setup
	SECCOMP_DENY(&fb, SYS_io_uring_setup);
#endif
#ifdef SYS_io_uring_enter
	SECCOMP_DENY(&fb, SYS_io_uring_enter);
#endif
#ifdef SYS_io_uring_register
	SECCOMP_DENY(&fb, SYS_io_uring_register);
#endif

	/*
	 * Without EXT_PERM_EXEC the process may still fork and run its
	 * own code, but it must not load a new program image.
	 */
	if (!(permissions & EXT_PERM_EXEC)) {
#ifdef SYS_execve
		SECCOMP_DENY(&fb, SYS_execve);
#endif
#ifdef SYS_execveat
		SECCOMP_DENY(&fb, SYS_execveat);
#endif
	}

	/*
	 * Network isolation: only AF_UNIX sockets are reachable and the
	 * socket setup/teardown calls are blocked.  socketpair(AF_UNIX)
	 * and recvfrom stay available so runtimes can manage children.
	 */
	if (!(permissions & EXT_PERM_NETWORK)) {
#ifdef SYS_socket
		SECCOMP_DENY_IP_SOCKET(&fb, SYS_socket);
#endif
#ifdef SYS_socketpair
		SECCOMP_DENY_IP_SOCKET(&fb, SYS_socketpair);
#endif
#ifdef SYS_connect
		SECCOMP_DENY(&fb, SYS_connect);
#endif
#ifdef SYS_bind
		SECCOMP_DENY(&fb, SYS_bind);
#endif
#ifdef SYS_listen
		SECCOMP_DENY(&fb, SYS_listen);
#endif
#ifdef SYS_accept
		SECCOMP_DENY(&fb, SYS_accept);
#endif
#ifdef SYS_accept4
		SECCOMP_DENY(&fb, SYS_accept4);
#endif
#ifdef SYS_sendto
		SECCOMP_DENY(&fb, SYS_sendto);
#endif
#ifdef SYS_sendmsg
		SECCOMP_DENY(&fb, SYS_sendmsg);
#endif
#ifdef SYS_sendmmsg
		SECCOMP_DENY(&fb, SYS_sendmmsg);
#endif
#ifdef SYS_recvmmsg
		SECCOMP_DENY(&fb, SYS_recvmmsg);
#endif
#ifdef SYS_getsockopt
		SECCOMP_DENY(&fb, SYS_getsockopt);
#endif
#ifdef SYS_setsockopt
		SECCOMP_DENY(&fb, SYS_setsockopt);
#endif
#ifdef SYS_getpeername
		SECCOMP_DENY(&fb, SYS_getpeername);
#endif
#ifdef SYS_getsockname
		SECCOMP_DENY(&fb, SYS_getsockname);
#endif
#ifdef SYS_shutdown
		SECCOMP_DENY(&fb, SYS_shutdown);
#endif
	}

	rc = fb_ret(&fb, SECCOMP_RET_ALLOW);
	if (rc < 0)
		goto fail;

	/*
	 * PR_SET_NO_NEW_PRIVS may already be set by sandbox_apply_fs
	 * (landlock).  Setting it again is harmless.
	 */
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
		rc = -errno;
		log_err("sandbox: PR_SET_NO_NEW_PRIVS failed: %s",
			strerror(errno));
		goto fail;
	}

	struct sock_fprog prog;

	prog.len = (unsigned short)fb.nelts;
	prog.filter = fb.elts;
	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0) {
		rc = -errno;
		log_err("sandbox: SECCOMP_MODE_FILTER failed: %s",
			strerror(errno));
		goto fail;
	}

	log_info("sandbox: seccomp denylist installed (%zu instructions, "
		 "perms=0x%x)", fb.nelts, permissions);
	morph_array_cleanup(&fb);
	return 0;

fail:
	morph_array_cleanup(&fb);
	return rc;
}

#undef SECCOMP_DENY
#undef SECCOMP_DENY_IP_SOCKET

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

int sandbox_start_isolated_session(void)
{
	if (setsid() < 0)
		MORPH_RETURN_ERRNO();
	return 0;
}

int sandbox_close_inherited_fds(void)
{
#if defined(__linux__) && defined(__NR_close_range)
	if (syscall(__NR_close_range, (unsigned int)(STDERR_FILENO + 1),
		    UINT_MAX, 0) == 0)
		return 0;
	if (errno != ENOSYS && errno != EINVAL)
		MORPH_RETURN_ERRNO();
#endif
	struct rlimit rl;
	rlim_t limit;

	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
		limit = rl.rlim_cur;
	else {
		long open_max = sysconf(_SC_OPEN_MAX);

		limit = open_max > 0 ? (rlim_t)open_max : (rlim_t)1024;
	}
	for (rlim_t fd = (rlim_t)(STDERR_FILENO + 1); fd < limit; fd++)
		(void)close((int)fd);
	return 0;
}

int sandbox_enter(struct sandbox_config *cfg)
{
	unsigned int effective_permissions;
	int rc;

	if (!cfg)
		return -EINVAL;
	effective_permissions = cfg->permissions;
	if (cfg->path_policy_enabled) {
		effective_permissions = 0;
		if (cfg->network_access)
			effective_permissions |= EXT_PERM_NETWORK;
		if (cfg->process_exec)
			effective_permissions |= EXT_PERM_EXEC;
		if (cfg->allow_pty)
			effective_permissions |= EXT_PERM_PTY;
		if (cfg->allow_process_info)
			effective_permissions |= EXT_PERM_PROCESS_INFO;
		if (cfg->allow_ipc)
			effective_permissions |= EXT_PERM_IPC;
		if (cfg->allow_temp)
			effective_permissions |= EXT_PERM_TEMP;
		if (cfg->write_paths_count > 0 || cfg->delete_paths_count > 0)
			effective_permissions |= EXT_PERM_FILESYS;
	}

	log_info("sandbox_enter: perms=0x%x mem=%dMB cpu=%ds fsize=%dMB nproc=%d",
		 cfg->permissions, cfg->max_memory_mb, cfg->max_cpu_seconds,
		 cfg->max_file_size_mb, cfg->max_processes);

	rc = sandbox_apply_env((const char **)cfg->allowed_env,
			       cfg->allowed_env_count,
			       effective_permissions);
	if (rc < 0)
		return rc;

	rc = sandbox_apply_rlimits(effective_permissions, cfg->max_memory_mb,
				   cfg->max_cpu_seconds,
				   cfg->max_file_size_mb,
				   cfg->max_processes,
				   cfg->max_open_files);
	if (rc < 0)
		return rc;

#ifdef __linux__
	if (cfg->path_policy_enabled)
		rc = sandbox_apply_path_policy(cfg);
	else
#endif
		rc = sandbox_apply_fs((const char **)cfg->allowed_paths,
				       cfg->allowed_paths_count,
				       effective_permissions);
	if (rc < 0)
		return rc;

#ifdef __linux__
	rc = sandbox_apply_seccomp(effective_permissions);
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
