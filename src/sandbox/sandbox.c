#include "sandbox.h"
#include "util/log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>

int sandbox_apply_rlimits(unsigned int permissions, int max_memory_mb, int max_cpu_seconds)
{
	struct rlimit rl;

	if (max_memory_mb > 0) {
#ifdef __linux__
		rl.rlim_cur = (rlim_t)max_memory_mb * 1024 * 1024;
		rl.rlim_max = (rlim_t)max_memory_mb * 1024 * 1024;
		if (setrlimit(RLIMIT_AS, &rl) != 0) {
			log_warn("sandbox: setrlimit RLIMIT_AS failed: %s",
				 strerror(errno));
		} else {
			log_info("sandbox: RLIMIT_AS set to %dMB", max_memory_mb);
		}
#else
		(void)max_memory_mb;
		log_info("sandbox: RLIMIT_AS skipped (not enforced on this OS)");
#endif
	}

	if (max_cpu_seconds > 0) {
		rl.rlim_cur = (rlim_t)max_cpu_seconds;
		rl.rlim_max = (rlim_t)max_cpu_seconds + 1;
		if (setrlimit(RLIMIT_CPU, &rl) != 0) {
			log_warn("sandbox: setrlimit RLIMIT_CPU failed: %s",
				 strerror(errno));
		} else {
			log_info("sandbox: RLIMIT_CPU set to %ds", max_cpu_seconds);
		}
	}

	rl.rlim_cur = 256;
	rl.rlim_max = 256;
	if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
		log_warn("sandbox: setrlimit RLIMIT_NOFILE failed: %s",
			 strerror(errno));
	} else {
		log_info("sandbox: RLIMIT_NOFILE set to 256");
	}

	(void)permissions;

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
	/* Probe for landlock support */
	int probe_fd = ll_create_ruleset(0);
	if (probe_fd < 0) {
		if (errno == EOPNOTSUPP || errno == ENOSYS) {
			log_info("sandbox: landlock not supported by kernel, "
				 "fs restrictions skipped");
			return 0;
		}
		log_warn("sandbox: landlock probe failed: %s, "
			 "fs restrictions skipped", strerror(errno));
		return 0;
	}
	close(probe_fd);

	/*
	 * Determine handled access rights.
	 *
	 * We always handle read+execute so that the default-deny policy
	 * blocks write operations unless explicitly allowed via rules.
	 *
	 * If EXT_PERM_FILESYS is NOT set, we create a read-only sandbox:
	 *   - read file/dir + execute are allowed everywhere
	 *   - write operations are blocked (not in handled_access_fs,
	 *     so they fall through to kernel default which is... allow)
	 *   Wait - landlock only restricts what's in handled_access_fs.
	 *   To block writes, we MUST include them in handled_access_fs
	 *   but NOT add any rules granting them.
	 *
	 * If EXT_PERM_FILESYS IS set with allowed_paths, we grant
	 * write access only under those paths.
	 *
	 * If EXT_PERM_FILESYS IS set without allowed_paths, we grant
	 * write access everywhere (no landlock rules for write).
	 *
	 * Strategy: always handle write rights in the ruleset so that
	 * landlock will enforce the default-deny on writes. Then add
	 * per-path rules that grant write access only where requested.
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

	uint64_t handled = read_access;
	int needs_write_rules = 0;

	if (permissions & EXT_PERM_FILESYS) {
		if (count > 0 && allowed_paths) {
			/*
			 * FILESYS requested with path restrictions:
			 * handle write access in ruleset (default-deny),
			 * then grant write on specific paths.
			 */
			handled |= write_access;
			needs_write_rules = 1;
		} else {
			/*
			 * FILESYS requested without path restrictions:
			 * don't handle write access in the ruleset,
			 * so writes are NOT restricted by landlock.
			 * (They may still be restricted by seccomp.)
			 */
		}
	} else {
		/*
		 * No FILESYS permission: handle write access in the
		 * ruleset without granting it anywhere = default-deny.
		 */
		handled |= write_access;
	}

	int ruleset_fd = ll_create_ruleset(handled);
	if (ruleset_fd < 0) {
		log_warn("sandbox: landlock create_ruleset failed: %s",
			 strerror(errno));
		return 0;
	}

	/*
	 * Add rules for each allowed path.
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
	 * PR_SET_NO_NEW_PRIVS must be set before landlock_restrict_self.
	 * This is also done in sandbox_apply_seccomp(), but we need it
	 * here first since landlock comes before seccomp in sandbox_enter().
	 */
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
		log_warn("sandbox: landlock: PR_SET_NO_NEW_PRIVS failed: %s",
			 strerror(errno));
		close(ruleset_fd);
		return 0;
	}

	if (ll_restrict_self(ruleset_fd) < 0) {
		log_warn("sandbox: landlock: restrict_self failed: %s",
			 strerror(errno));
		close(ruleset_fd);
		return 0;
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

	/* Always allow file reading */
	n = snprintf(sbpl + off, remaining,
		     "(allow file-read*)\n");
	if (n > 0 && (size_t)n < remaining)
		off += n;
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
		/* Non-fatal: degrade to rlimits only */
		return 0;
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

struct filter_builder {
	struct sock_filter *insns;
	size_t count;
	size_t cap;
};

static int fb_init(struct filter_builder *fb, size_t initial_cap)
{
	fb->insns = calloc(initial_cap, sizeof(struct sock_filter));
	if (!fb->insns)
		return -ENOMEM;
	fb->count = 0;
	fb->cap = initial_cap;
	return 0;
}

static void fb_free(struct filter_builder *fb)
{
	free(fb->insns);
	fb->insns = NULL;
	fb->count = 0;
	fb->cap = 0;
}

static int fb_append(struct filter_builder *fb, struct sock_filter insn)
{
	if (fb->count >= fb->cap) {
		size_t new_cap = fb->cap * 2;
		struct sock_filter *new_insns =
			realloc(fb->insns, new_cap * sizeof(*new_insns));
		if (!new_insns)
			return -ENOMEM;
		fb->insns = new_insns;
		fb->cap = new_cap;
	}
	fb->insns[fb->count++] = insn;
	return 0;
}

static int fb_allow(struct filter_builder *fb, int nr)
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
	struct filter_builder fb;
	int rc;

	rc = fb_init(&fb, 128);
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
	}

	if (permissions & EXT_PERM_EXEC) {
		rc = fb_allow(&fb, SYS_execve);     if (rc < 0) goto fail;
#ifdef __NR_execveat
		rc = fb_allow(&fb, __NR_execveat);  if (rc < 0) goto fail;
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
	}

	rc = fb_append(&fb, (struct sock_filter)BPF_STMT(
		BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
	if (rc < 0)
		goto fail;

	/*
	 * PR_SET_NO_NEW_PRIVS may have already been set by
	 * sandbox_apply_fs (landlock). Setting it again is harmless.
	 */
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
		log_err("sandbox: PR_SET_NO_NEW_PRIVS failed: %s",
			strerror(errno));
		rc = -errno;
		goto fail;
	}

	struct sock_fprog prog;
	prog.len = (unsigned short)fb.count;
	prog.filter = fb.insns;

	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0) {
		log_err("sandbox: SECCOMP_MODE_FILTER failed: %s",
			strerror(errno));
		rc = -errno;
		goto fail;
	}

	log_info("sandbox: seccomp-bpf filter installed (%zu instructions, "
		 "perms=0x%x)", fb.count, permissions);
	fb_free(&fb);
	return 0;

fail:
	fb_free(&fb);
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

#ifndef __APPLE__

int sandbox_enter_darwin(struct sandbox_config *cfg)
{
	if (!cfg)
		return -EINVAL;
	(void)cfg;
	return 0;
}

#endif /* !__APPLE__ */

int sandbox_enter(struct sandbox_config *cfg)
{
	if (!cfg)
		return -EINVAL;

	log_info("sandbox_enter: perms=0x%x mem=%dMB cpu=%ds",
		 cfg->permissions, cfg->max_memory_mb, cfg->max_cpu_seconds);

	int rc;

	rc = sandbox_apply_rlimits(cfg->permissions, cfg->max_memory_mb, cfg->max_cpu_seconds);
	if (rc < 0)
		return rc;

	rc = sandbox_apply_fs((const char **)cfg->allowed_paths,
			       cfg->allowed_paths_count,
			       cfg->permissions);
	if (rc < 0)
		log_warn("sandbox: fs restriction failed: %d", rc);

#ifdef __linux__
	rc = sandbox_apply_seccomp(cfg->permissions);
	if (rc < 0)
		return rc;
#elif defined(__APPLE__)
	rc = sandbox_enter_darwin(cfg);
	if (rc < 0)
		log_warn("sandbox: macOS sandbox failed (non-fatal): %d", rc);
#else
	log_warn("sandbox: no platform-specific isolation available");
#endif

	return 0;
}