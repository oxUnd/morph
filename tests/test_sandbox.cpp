#include <gtest/gtest.h>
extern "C" {
#include "sandbox/sandbox.h"
}
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits.h>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

/*
 * Run a callable in a forked child so we don't pollute the test
 * process's environment / rlimits / sandbox state. Returns the
 * child exit code (or 128+signal if killed).
 */
template <typename Fn>
static int run_in_child(Fn fn)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int rc = fn();
		_exit(rc & 0xff);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}

/* ------------------------------------------------------------------
 * sandbox_apply_env
 * ------------------------------------------------------------------ */

TEST(SandboxEnvTest, FiltersOutNonAllowedVars)
{
	int rc = run_in_child([]() {
		setenv("MORPH_TEST_SECRET", "topsecret", 1);
		setenv("MORPH_TEST_KEEP", "ok", 1);

		const char *allow[] = {"MORPH_TEST_KEEP"};
		int rv = sandbox_apply_env(allow, 1, 0);
		if (rv != 0)
			return 10;

		if (getenv("MORPH_TEST_SECRET") != NULL)
			return 20;
		const char *kept = getenv("MORPH_TEST_KEEP");
		if (!kept || strcmp(kept, "ok") != 0)
			return 21;
		/* Essential vars must survive */
		if (getenv("PATH") == NULL)
			return 22;
		return 0;
	});
	EXPECT_EQ(rc, 0);
}

TEST(SandboxEnvTest, EnvPermBypassesFilter)
{
	int rc = run_in_child([]() {
		setenv("MORPH_TEST_BYPASS", "v", 1);
		int rv = sandbox_apply_env(NULL, 0, EXT_PERM_ENV);
		if (rv != 0)
			return 10;
		if (getenv("MORPH_TEST_BYPASS") == NULL)
			return 11;
		return 0;
	});
	EXPECT_EQ(rc, 0);
}

TEST(SandboxEnvTest, KeepsLcLocaleFamily)
{
	int rc = run_in_child([]() {
		setenv("LC_FOOBAR", "x", 1);
		setenv("RANDOM_VAR_XYZ", "x", 1);
		int rv = sandbox_apply_env(NULL, 0, 0);
		if (rv != 0)
			return 10;
		if (getenv("LC_FOOBAR") == NULL)
			return 11;
		if (getenv("RANDOM_VAR_XYZ") != NULL)
			return 12;
		return 0;
	});
	EXPECT_EQ(rc, 0);
}

/* ------------------------------------------------------------------
 * sandbox_apply_rlimits
 * ------------------------------------------------------------------ */

TEST(SandboxRlimitTest, FsizeBlocksLargeWrites)
{
	int rc = run_in_child([]() {
		/* Ignore SIGXFSZ so the child returns the write() error
		 * instead of being killed. */
		signal(SIGXFSZ, SIG_IGN);

		int rv = sandbox_apply_rlimits(0, 0, 0, 1, 0, 0);
		if (rv != 0)
			return 10;

		char tmpl[] = "/tmp/morph_sb_fsize_XXXXXX";
		int fd = mkstemp(tmpl);
		if (fd < 0)
			return 11;
		unlink(tmpl);

		/* 1 MB write should succeed. */
		std::string buf(1024 * 1024, 'a');
		ssize_t w = write(fd, buf.data(), buf.size());
		if (w != (ssize_t)buf.size()) {
			close(fd);
			return 12;
		}

		/* Next byte beyond 1 MB must fail (EFBIG). */
		errno = 0;
		w = write(fd, "x", 1);
		int err = errno;
		close(fd);
		if (w >= 0)
			return 13;
		if (err != EFBIG)
			return 14;
		return 0;
	});
	EXPECT_EQ(rc, 0);
}

TEST(SandboxRlimitTest, CoreDumpDisabled)
{
	int rc = run_in_child([]() {
		int rv = sandbox_apply_rlimits(0, 0, 0, 0, 0, 0);
		if (rv != 0)
			return 10;
		struct rlimit rl;
		if (getrlimit(RLIMIT_CORE, &rl) != 0)
			return 11;
		if (rl.rlim_cur != 0)
			return 12;
		return 0;
	});
	EXPECT_EQ(rc, 0);
}

TEST(SandboxRlimitTest, NofileDefaultsTo1024)
{
	int rc = run_in_child([]() {
		int rv = sandbox_apply_rlimits(0, 0, 0, 0, 0, 0);
		if (rv != 0)
			return 10;
		struct rlimit rl;
		if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
			return 11;
		if (rl.rlim_cur != 1024)
			return 12;
		return 0;
	});
	EXPECT_EQ(rc, 0);
}

TEST(SandboxRlimitTest, NofileCustomLimit)
{
	int rc = run_in_child([]() {
		int rv = sandbox_apply_rlimits(0, 0, 0, 0, 0, 4096);
		if (rv != 0)
			return 10;
		struct rlimit rl;
		if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
			return 11;
		if (rl.rlim_cur != 4096)
			return 12;
		return 0;
	});
	EXPECT_EQ(rc, 0);
}

TEST(SandboxFdTest, ClosesDescriptorsAboveStandardStreams)
{
	int rc = run_in_child([]() {
		int fds[2];

		if (pipe(fds) != 0)
			return 10;
		if (sandbox_close_inherited_fds() != 0)
			return 11;
		errno = 0;
		if (fcntl(fds[0], F_GETFD) != -1 || errno != EBADF)
			return 12;
		errno = 0;
		if (fcntl(fds[1], F_GETFD) != -1 || errno != EBADF)
			return 13;
		return 0;
	});

	EXPECT_EQ(rc, 0);
}

TEST(SandboxSessionTest, StartsNewTerminalSession)
{
	pid_t parent_session = getsid(0);
	int rc = run_in_child([&]() {
		if (sandbox_start_isolated_session() != 0)
			return 10;
		if (getsid(0) != getpid())
			return 11;
		if (getsid(0) == parent_session)
			return 12;
		return 0;
	});

	EXPECT_EQ(rc, 0);
}

/* ------------------------------------------------------------------
 * sandbox_enter (end-to-end)
 *
 * On macOS the SBPL profile (deny default) is applied. Without
 * EXT_PERM_FILESYS, file writes outside allowed_paths must fail.
 * On Linux, seccomp+landlock should achieve the same. Isolation
 * initialization now fails closed rather than degrading silently.
 * ------------------------------------------------------------------ */

#if defined(__APPLE__)
TEST(SandboxEnterTest, MacOSMetadataAccessDoesNotGrantFileContents)
{
	char tmpl[] = "/tmp/morph_sb_read_deny_XXXXXX";
	int fd = mkstemp(tmpl);

	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, "secret", 6), 6);
	close(fd);
	int rc = run_in_child([&]() {
		char *read_paths[] = { (char *)"/dev" };
		struct sandbox_config cfg = {};

		cfg.path_policy_enabled = 1;
		cfg.read_paths = read_paths;
		cfg.read_paths_count = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		int read_fd = open(tmpl, O_RDONLY);
		if (read_fd >= 0) {
			close(read_fd);
			return 11;
		}
		return 0;
	});

	EXPECT_EQ(rc, 0);
	unlink(tmpl);
}

TEST(SandboxEnterTest, MacOSDeniesUnauthorizedWrite)
{
	int rc = run_in_child([]() {
		/* Pre-create the target so we just need write access. */
		char tmpl[] = "/tmp/morph_sb_deny_XXXXXX";
		int fd = mkstemp(tmpl);
		if (fd < 0)
			return 10;
		close(fd);

		struct sandbox_config cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.permissions = 0; /* No FILESYS perm => writes denied */

		if (sandbox_enter(&cfg) != 0) {
			unlink(tmpl);
			return 11;
		}

		fd = open(tmpl, O_WRONLY);
		if (fd >= 0) {
			close(fd);
			unlink(tmpl);
			return 12;
		}
		unlink(tmpl);
		return 0;
	});
	EXPECT_EQ(rc, 0);
}

TEST(SandboxEnterTest, MacOSAllowsOptInProcessTempReadWriteCreateAndChmod)
{
	char temp_dir[PATH_MAX];
	char temp_path[PATH_MAX];
	size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, temp_dir,
				 sizeof(temp_dir));

	ASSERT_GT(length, 0U);
	ASSERT_LE(length, sizeof(temp_dir));
	ASSERT_GT(snprintf(temp_path, sizeof(temp_path),
			   "%smorph_sb_temp_XXXXXX", temp_dir), 0);
	int fd = mkstemp(temp_path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, "before", 6), 6);
	close(fd);

	int rc = run_in_child([&]() {
		struct sandbox_config cfg = {};
		char created_path[PATH_MAX];
		cfg.allow_temp = 1;
		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;

		if (sandbox_enter(&cfg) != 0)
			return 10;
		int temp_fd = open(temp_path, O_RDWR);
		if (temp_fd < 0)
			return 11;
		char value[7] = {};
		if (read(temp_fd, value, 6) != 6 || strcmp(value, "before") != 0) {
			close(temp_fd);
			return 12;
		}
		if (lseek(temp_fd, 0, SEEK_SET) < 0 ||
		    write(temp_fd, "after!", 6) != 6) {
			close(temp_fd);
			return 13;
		}
		close(temp_fd);
		if (chmod(temp_path, 0640) != 0)
			return 14;
		if (snprintf(created_path, sizeof(created_path), "%s.created",
			     temp_path) <= 0)
			return 15;
		temp_fd = open(created_path, O_CREAT | O_WRONLY, 0600);
		if (temp_fd < 0)
			return 16;
		close(temp_fd);
		return 0;
	});

	EXPECT_EQ(rc, 0);
	struct stat st = {};
	EXPECT_EQ(stat(temp_path, &st), 0);
	EXPECT_EQ(st.st_mode & 0777, 0640);
	char created_path[PATH_MAX];
	ASSERT_GT(snprintf(created_path, sizeof(created_path), "%s.created",
			   temp_path), 0);
	unlink(created_path);
	unlink(temp_path);
}

TEST(SandboxEnterTest, MacOSAllowsOptInPtyAllocation)
{
	int rc = run_in_child([]() {
		struct sandbox_config cfg = {};

		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		cfg.allow_pty = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		int master = posix_openpt(O_RDWR | O_NOCTTY);
		if (master < 0)
			return 11;
		if (grantpt(master) != 0 || unlockpt(master) != 0) {
			close(master);
			return 12;
		}
		close(master);
		return 0;
	});

	EXPECT_EQ(rc, 0);
}
#endif

#if defined(__linux__)
TEST(SandboxEnterTest, LinuxAllowsOptInPtyAllocation)
{
	int rc = run_in_child([]() {
		struct sandbox_config cfg = {};

		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		cfg.allow_pty = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		int master = posix_openpt(O_RDWR | O_NOCTTY);
		if (master < 0)
			return 11;
		if (grantpt(master) != 0 || unlockpt(master) != 0) {
			close(master);
			return 12;
		}
		close(master);
		return 0;
	});

	EXPECT_EQ(rc, 0);
}

TEST(SandboxEnterTest, LinuxAllowsOptInPosixSharedMemory)
{
	char shm_name[64];

	ASSERT_GT(snprintf(shm_name, sizeof(shm_name), "/morph_sb_%ld",
			   static_cast<long>(getpid())), 0);
	shm_unlink(shm_name);
	int rc = run_in_child([&]() {
		struct sandbox_config cfg = {};

		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		cfg.allow_ipc = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
		if (fd < 0)
			return 11;
		if (ftruncate(fd, 4096) != 0) {
			close(fd);
			shm_unlink(shm_name);
			return 12;
		}
		close(fd);
		if (shm_unlink(shm_name) != 0)
			return 13;
		return 0;
	});

	EXPECT_EQ(rc, 0);
	shm_unlink(shm_name);
}

TEST(SandboxEnterTest, LinuxProcessInfoControlsOtherProcEntries)
{
	char parent_cmdline[PATH_MAX];

	ASSERT_GT(snprintf(parent_cmdline, sizeof(parent_cmdline),
			   "/proc/%ld/cmdline", static_cast<long>(getpid())), 0);
	int rc = run_in_child([&]() {
		struct sandbox_config cfg = {};

		cfg.path_policy_enabled = 1;
		cfg.process_exec = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		int fd = open(parent_cmdline, O_RDONLY);
		if (fd >= 0) {
			close(fd);
			return 11;
		}
		return 0;
	});

	EXPECT_EQ(rc, 0);
}

TEST(SandboxEnterTest, LinuxProcessInfoAllowsOtherProcEntries)
{
	char parent_cmdline[PATH_MAX];

	ASSERT_GT(snprintf(parent_cmdline, sizeof(parent_cmdline),
			   "/proc/%ld/cmdline", static_cast<long>(getpid())), 0);
	int rc = run_in_child([&]() {
		struct sandbox_config cfg = {};

		cfg.path_policy_enabled = 1;
		cfg.process_exec = 1;
		cfg.allow_process_info = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		int fd = open(parent_cmdline, O_RDONLY);
		if (fd < 0)
			return 11;
		close(fd);
		return 0;
	});

	EXPECT_EQ(rc, 0);
}
#endif

TEST(SandboxEnterTest, RejectsNullConfig)
{
	EXPECT_EQ(sandbox_enter(NULL), -EINVAL);
}

TEST(SandboxEnterTest, PathPolicySeparatesWriteAndDelete)
{
	char write_dir[] = "/tmp/morph_sb_write_XXXXXX";
	char delete_dir[] = "/tmp/morph_sb_delete_XXXXXX";
	char write_file[PATH_MAX];
	char delete_file[PATH_MAX];
	char resolved_write_dir[PATH_MAX];
	char resolved_delete_dir[PATH_MAX];

	ASSERT_NE(mkdtemp(write_dir), nullptr);
	ASSERT_NE(mkdtemp(delete_dir), nullptr);
	ASSERT_NE(realpath(write_dir, resolved_write_dir), nullptr);
	ASSERT_NE(realpath(delete_dir, resolved_delete_dir), nullptr);
	ASSERT_GT(snprintf(write_file, sizeof(write_file), "%s/file", write_dir),
		0);
	ASSERT_GT(snprintf(delete_file, sizeof(delete_file), "%s/file",
		delete_dir), 0);
	int fd = open(write_file, O_CREAT | O_WRONLY, 0600);
	ASSERT_GE(fd, 0);
	close(fd);
	fd = open(delete_file, O_CREAT | O_WRONLY, 0600);
	ASSERT_GE(fd, 0);
	close(fd);
	int rc = run_in_child([&]() {
		char *write_paths[] = {write_dir, resolved_write_dir};
		char *delete_paths[] = {delete_dir, resolved_delete_dir};
		struct sandbox_config cfg = {};

		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		cfg.write_paths = write_paths;
		cfg.write_paths_count = 2;
		cfg.delete_paths = delete_paths;
		cfg.delete_paths_count = 2;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		int write_fd = open(write_file, O_WRONLY);
		if (write_fd < 0)
			return 11;
		if (write(write_fd, "x", 1) != 1) {
			close(write_fd);
			return 12;
		}
		close(write_fd);
		if (unlink(write_file) == 0)
			return 13;
		write_fd = open(delete_file, O_WRONLY);
		if (write_fd >= 0) {
			close(write_fd);
			return 14;
		}
		if (unlink(delete_file) != 0)
			return 15;
		return 0;
	});
	EXPECT_EQ(rc, 0);
	unlink(write_file);
	unlink(delete_file);
	rmdir(write_dir);
	rmdir(delete_dir);
}

#if defined(__linux__) || defined(__APPLE__)
TEST(SandboxEnterTest, PathPolicyAllowsDevNullWrite)
{
	char denied_file[] = "/tmp/morph_sb_devnull_XXXXXX";
	int fd = mkstemp(denied_file);

	ASSERT_GE(fd, 0);
	close(fd);
	int rc = run_in_child([&]() {
		struct sandbox_config cfg = {};

		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		int null_fd = open("/dev/null", O_WRONLY | O_TRUNC);
		if (null_fd < 0)
			return 11;
		if (write(null_fd, "discard", 7) != 7) {
			close(null_fd);
			return 12;
		}
		close(null_fd);
		int denied_fd = open(denied_file, O_WRONLY | O_TRUNC);
		if (denied_fd >= 0) {
			close(denied_fd);
			return 13;
		}
		return 0;
	});

	EXPECT_EQ(rc, 0);
	unlink(denied_file);
}

TEST(SandboxEnterTest, LegacyPolicyAllowsDevNullWrite)
{
	int rc = run_in_child([]() {
		struct sandbox_config cfg = {};

		if (sandbox_enter(&cfg) != 0)
			return 10;
		int null_fd = open("/dev/null", O_WRONLY | O_TRUNC);
		if (null_fd < 0)
			return 11;
		if (write(null_fd, "discard", 7) != 7) {
			close(null_fd);
			return 12;
		}
		close(null_fd);
		return 0;
	});

	EXPECT_EQ(rc, 0);
}
#endif

static int create_loopback_listener(uint16_t *port)
{
	struct sockaddr_in address = {};
	socklen_t length = sizeof(address);
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	if (bind(fd, reinterpret_cast<struct sockaddr *>(&address),
		 sizeof(address)) != 0 ||
	    getsockname(fd, reinterpret_cast<struct sockaddr *>(&address),
			&length) != 0 || listen(fd, 1) != 0) {
		close(fd);
		return -1;
	}
	*port = ntohs(address.sin_port);
	return fd;
}

static int sandbox_loopback_connect(uint16_t port, int network_access)
{
	return run_in_child([=]() {
		struct sandbox_config cfg = {};
		struct sockaddr_in address = {};

		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		cfg.network_access = network_access;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			return 11;
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		int rc = connect(fd,
			reinterpret_cast<struct sockaddr *>(&address),
			sizeof(address));
		close(fd);
		return rc == 0 ? 0 : 12;
	});
}

TEST(SandboxEnterTest, PathPolicyDeniesNetworkByDefault)
{
	uint16_t port = 0;
	int listener = create_loopback_listener(&port);

	ASSERT_GE(listener, 0);
	EXPECT_NE(sandbox_loopback_connect(port, 0), 0);
	close(listener);
}

TEST(SandboxEnterTest, PathPolicyAllowsConfiguredNetwork)
{
	uint16_t port = 0;
	int listener = create_loopback_listener(&port);

	ASSERT_GE(listener, 0);
	EXPECT_EQ(sandbox_loopback_connect(port, 1), 0);
	close(listener);
}
