#include "sandbox.h"
#include "util/log.h"
#include <errno.h>
#include <stdlib.h>
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

int sandbox_apply_fs(const char **allowed_paths, int count)
{
	(void)allowed_paths;
	(void)count;
	log_info("sandbox: filesystem path restrictions not yet enforced "
		 "(requires landlock on Linux or sandbox-exec on macOS)");
	return 0;
}

#ifdef __linux__

#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
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
	rc = fb_allow(&fb, SYS_poll);       if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_mremap);     if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_nanosleep);  if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_clock_gettime); if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_getpid);     if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_sendfile);   if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_dup);       if (rc < 0) goto fail;
	rc = fb_allow(&fb, SYS_dup2);       if (rc < 0) goto fail;
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

#if defined(__APPLE__) || !defined(__linux__)

int sandbox_apply_seccomp(unsigned int permissions)
{
	(void)permissions;
	log_info("sandbox: seccomp-bpf not available on this platform, "
		 "using rlimits only");
	return 0;
}

int sandbox_enter_darwin(struct sandbox_config *cfg)
{
	if (!cfg)
		return -EINVAL;

	log_info("sandbox_enter_darwin: macOS sandbox-exec integration is P1 "
		 "(perms=0x%x)", cfg->permissions);
	(void)cfg;
	return 0;
}

#endif /* __APPLE__ || !__linux__ */

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
			       cfg->allowed_paths_count);
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