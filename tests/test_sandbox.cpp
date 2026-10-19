#include <gtest/gtest.h>
extern "C" {
#include "sandbox/sandbox.h"
#include "sandbox/tool_paths.h"
#include "agent/tool_context.h"
#include "agent/tool.h"
#include "agent/tools/exec_tool.h"
#include "agent/tools/request_permissions.h"
extern char **environ;
}
#include <cerrno>
#include <filesystem>
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
#if defined(__APPLE__)
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#endif
#if defined(__linux__)
#include <sys/mman.h>
#include <sys/syscall.h>
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
TEST(SandboxEnterTest, MacOSDevelopmentRuntimeAllowsSurfaceAndHostProcessProbe)
{
	auto check_surface = []() {
		int dimension = 16;
		int bytes = 4;
		CFNumberRef size = CFNumberCreate(NULL, kCFNumberIntType, &dimension);
		CFNumberRef pixel = CFNumberCreate(NULL, kCFNumberIntType, &bytes);
		if (!size || !pixel)
			return 12;
		const void *keys[] = {kIOSurfaceWidth, kIOSurfaceHeight,
			kIOSurfaceBytesPerElement};
		const void *values[] = {size, size, pixel};
		CFDictionaryRef properties = CFDictionaryCreate(NULL, keys, values, 3,
			&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFRelease(size);
		CFRelease(pixel);
		if (!properties)
			return 13;
		IOSurfaceRef surface = IOSurfaceCreate(properties);
		CFRelease(properties);
		if (!surface)
			return 14;
		IOReturn locked = IOSurfaceLock(surface, 0, NULL);
		if (locked == kIOReturnSuccess)
			IOSurfaceUnlock(surface, 0, NULL);
		CFRelease(surface);
		return locked == kIOReturnSuccess ? 0 : 15;
	};
	/* Re-exec: IOKit connections and Objective-C state cannot cross fork. */
	::testing::FLAGS_gtest_death_test_style = "threadsafe";
	EXPECT_EXIT({
	int rc = [&]() {
		pid_t parent = getppid();
		struct sandbox_config cfg = {};
		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		cfg.development_runtime = 1;
		cfg.allow_temp = 1;
		setenv("MORPH_TEST_DEVELOPMENT_ENV", "retained", 1);
		if (sandbox_enter(&cfg) != 0)
			return 10;
		const char *env = getenv("MORPH_TEST_DEVELOPMENT_ENV");
		if (!env || strcmp(env, "retained") != 0)
			return 16;
		/* Signal zero checks permission without delivering a signal. */
		if (kill(parent, 0) != 0)
			return 11;
		return check_surface();
	}();
	_exit(rc);
	}, ::testing::ExitedWithCode(0), "");
}

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

/*
 * The seccomp policy is a denylist with a default ALLOW action.  These
 * tests guard against a regression to the old allowlist behaviour, in
 * which any syscall not explicitly listed failed with ENOSYS or killed
 * the process.
 */
#if defined(__linux__)
TEST(SandboxSeccompTest, AllowsUnlistedSyscalls)
{
	int rc = run_in_child([]() {
		struct sandbox_config cfg = {};

		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		errno = 0;
		long rv = syscall(SYS_getcpu, NULL, NULL, NULL);
		if (rv != 0)
			return 11;
		return 0;
	});

	EXPECT_EQ(rc, 0);
}

TEST(SandboxSeccompTest, DeniesDangerousSyscallsWithEperm)
{
#if defined(SYS_io_uring_setup)
	int rc = run_in_child([]() {
		struct sandbox_config cfg = {};

		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		errno = 0;
		long rv = syscall(SYS_io_uring_setup, 1, NULL);
		if (rv != -1 || errno != EPERM)
			return 11;
		return 0;
	});

	EXPECT_EQ(rc, 0);
#else
	GTEST_SKIP() << "SYS_io_uring_setup unavailable";
#endif
}

TEST(SandboxSeccompTest, AllowsUnixSocketpairWithoutNetwork)
{
	int rc = run_in_child([]() {
		struct sandbox_config cfg = {};
		int fds[2];

		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
			return 11;
		close(fds[0]);
		close(fds[1]);
		return 0;
	});

	EXPECT_EQ(rc, 0);
}

TEST(SandboxSeccompTest, DeniesInternetSocketsWithoutNetwork)
{
	int rc = run_in_child([]() {
		struct sandbox_config cfg = {};

		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		errno = 0;
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd >= 0) {
			close(fd);
			return 11;
		}
		if (errno != EPERM)
			return 12;
		return 0;
	});

	EXPECT_EQ(rc, 0);
}

TEST(SandboxSeccompTest, RunsPythonIfAvailable)
{
	if (access("/usr/bin/python3", X_OK) != 0)
		GTEST_SKIP() << "python3 not installed";

	int rc = run_in_child([]() {
		struct sandbox_config cfg = {};
		char *write_paths[] = { (char *)"/tmp" };

		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		cfg.process_exec = 1;
		cfg.allow_temp = 1;
		cfg.write_paths = write_paths;
		cfg.write_paths_count = 1;
		cfg.delete_paths = write_paths;
		cfg.delete_paths_count = 1;
		if (sandbox_enter(&cfg) != 0)
			return 10;
		execl("/usr/bin/python3", "python3", "-c",
		      "import ssl, ctypes, hashlib", (char *)NULL);
		return 11;
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

/* Prepare in an isolated process so the real home is never modified. */
TEST(SandboxToolPathsTest, CacheLifecycleAndIsolation)
{
	char temp[] = "/tmp/morph_tool_paths_XXXXXX";
	ASSERT_NE(mkdtemp(temp), nullptr);
	std::string home = std::string(temp) + "/home";
	ASSERT_EQ(mkdir(home.c_str(), 0700), 0);
	int rc = run_in_child([&]() {
		/* Do not inherit tool locations from the developer machine. */
		while (environ && *environ) {
			std::string name(*environ);
			name.resize(name.find('='));
			if (unsetenv(name.c_str()) != 0)
				return 30;
		}
		setenv("HOME", home.c_str(), 1);
		setenv("XDG_CACHE_HOME", (home + "/cache").c_str(), 1);
		setenv("XDG_CONFIG_HOME", (home + "/config").c_str(), 1);
		setenv("XDG_DATA_HOME", (home + "/data").c_str(), 1);
		setenv("XDG_STATE_HOME", (home + "/state").c_str(), 1);
		setenv("XDG_RUNTIME_DIR", (home + "/runtime").c_str(), 1);
		if (mkdir((home + "/runtime").c_str(), 0700) != 0)
			return 23;
		setenv("npm_config_cache", (home + "/custom-npm").c_str(), 1);
		struct arena *arena = arena_create(8192);
		if (!arena)
			return 10;
		struct sandbox_config cfg = {};
		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		if (sandbox_prepare_tool_paths(&cfg, arena) != 0)
			return 11;
		std::string cache = home + "/custom-npm";
		std::string pip = home + "/cache/pip";
		struct stat st;
		if (stat(cache.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
		    stat(pip.c_str(), &st) != 0)
			return 12;
		/* Invalid overrides must not widen the policy or create cwd caches. */
		setenv("npm_config_cache", "/", 1);
		setenv("PIP_CACHE_DIR", "relative-cache", 1);
		struct sandbox_config invalid = {};
		invalid.path_policy_enabled = 1;
		if (sandbox_prepare_tool_paths(&invalid, arena) != 0)
			return 21;
		for (int i = 0; i < invalid.write_paths_count; i++) {
			if (strcmp(invalid.write_paths[i], "/") == 0 ||
			    strstr(invalid.write_paths[i], "relative-cache"))
				return 22;
		}
		if (sandbox_enter(&cfg) != 0)
			return 13;
		const char *shared[] = {
			".cache", ".config", ".local/share", ".local/state",
			"cache", "config", "data", "state", "runtime",
			".local/bin", ".local/lib", ".local/include",
			".cargo", ".rustup", ".gradle", ".m2", ".sbt", ".ivy2",
			"go", ".dotnet", ".nuget", ".nvm", ".volta", ".bun",
			".pyenv", ".conda", "miniforge3", ".gem", ".bundle",
			".rbenv", ".conan2", ".vcpkg", ".asdf", ".android",
			".julia", ".stack", ".cabal", ".mix", ".hex",
			".httpie", ".aria2", ".lftp", ".ssh/controlmasters",
			".morph/skills", ".agents/skills", ".codex", ".claude",
			".gemini", ".continue", ".cursor", ".aider", ".opencode",
			".ollama", ".lmstudio", ".triton",
#ifdef __APPLE__
			"Library/Caches", "Library/Developer", "Library/MobileDevice",
			"Library/Application Support/Xcode", "Library/Logs/CoreSimulator",
#endif
		};
		for (const char *dir : shared) {
			std::string subdir = home + "/" + dir + "/new-tool";
			if (mkdir(subdir.c_str(), 0700) != 0)
				return 24;
			std::string first_path = subdir + "/state.tmp";
			std::string final_path = subdir + "/state";
			int shared_fd = open(first_path.c_str(), O_CREAT | O_RDWR, 0600);
			if (shared_fd < 0)
				return 25;
			char value = 0;
			if (write(shared_fd, "x", 1) != 1 ||
			    lseek(shared_fd, 0, SEEK_SET) < 0 ||
			    read(shared_fd, &value, 1) != 1 || value != 'x')
				return 26;
			close(shared_fd);
			if (rename(first_path.c_str(), final_path.c_str()) != 0 ||
			    unlink(final_path.c_str()) != 0 || rmdir(subdir.c_str()) != 0)
				return 27;
		}
		int lock_fd = open((home + "/.cargo/.package-cache").c_str(),
			O_CREAT | O_WRONLY, 0600);
		if (lock_fd < 0)
			return 19;
		close(lock_fd);
		int config_fd = open((home + "/.cargo/config.toml").c_str(),
			O_CREAT | O_WRONLY, 0600);
		if (config_fd < 0)
			return 20;
		close(config_fd);
		if (unlink((home + "/.cargo/config.toml").c_str()) != 0)
			return 31;
		std::string first = cache + "/download.tmp";
		std::string second = cache + "/download";
		int fd = open(first.c_str(), O_CREAT | O_WRONLY, 0600);
		if (fd < 0)
			return 14;
		if (write(fd, "cache", 5) != 5)
			return 15;
		close(fd);
		if (rename(first.c_str(), second.c_str()) != 0 ||
		    unlink(second.c_str()) != 0)
			return 16;
		fd = open((home + "/.npmrc").c_str(), O_CREAT | O_WRONLY, 0600);
		if (fd >= 0) {
			close(fd);
			return 17;
		}
		fd = open((home + "/.morph/config.toml").c_str(),
			O_CREAT | O_WRONLY, 0600);
		if (fd >= 0) {
			close(fd);
			return 32;
		}
		fd = open((home + "/unrelated").c_str(), O_CREAT | O_WRONLY, 0600);
		if (fd >= 0) {
			close(fd);
			return 18;
		}
		arena_destroy(arena);
		return 0;
	});
	EXPECT_EQ(rc, 0);
	/* This path comes exclusively from mkdtemp. */
	std::string cleanup = std::string("rm -rf ") + temp;
	EXPECT_EQ(system(cleanup.c_str()), 0);
}

TEST(SandboxToolPathsTest, RejectsInvalidArguments)
{
	struct sandbox_config cfg = {};
	struct arena *arena = arena_create(1024);
	ASSERT_NE(arena, nullptr);
	EXPECT_EQ(sandbox_prepare_tool_paths(nullptr, arena), -EINVAL);
	EXPECT_EQ(sandbox_prepare_tool_paths(&cfg, nullptr), -EINVAL);
	EXPECT_EQ(sandbox_prepare_tool_paths(&cfg, arena), -EINVAL);
	arena_destroy(arena);
}

TEST(SandboxEnterTest, VarTmpFollowsTempCapability)
{
	char dir[] = "/var/tmp/morph_sb_temp_XXXXXX";
	ASSERT_NE(mkdtemp(dir), nullptr);
	std::string path = std::string(dir) + "/state";
	for (int allow : {0, 1}) {
		int rc = run_in_child([&]() {
			struct sandbox_config cfg = {};
			cfg.path_policy_enabled = 1;
			cfg.read_all = 1;
			cfg.allow_temp = allow;
			if (sandbox_enter(&cfg) != 0)
				return 10;
			int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
			if (!allow)
				return fd < 0 ? 0 : 11;
			if (fd < 0)
				return 12;
			close(fd);
			return unlink(path.c_str()) == 0 ? 0 : 13;
		});
		EXPECT_EQ(rc, 0) << "allow_temp=" << allow;
	}
	unlink(path.c_str());
	rmdir(dir);
}

TEST(SandboxEnterTest, CustomTmpdirFollowsTempCapability)
{
	const char *home = getenv("HOME");
	ASSERT_NE(home, nullptr);
	char dir[PATH_MAX];
	ASSERT_LT(snprintf(dir, sizeof(dir), "%s/.morph_test_tmp_XXXXXX", home),
		(int)sizeof(dir));
	ASSERT_NE(mkdtemp(dir), nullptr);
	char resolved[PATH_MAX];
	ASSERT_NE(realpath(dir, resolved), nullptr);
	if (strncmp(resolved, "/tmp/", 5) == 0 ||
	    strncmp(resolved, "/private/tmp/", 13) == 0 ||
	    strncmp(resolved, "/var/tmp/", 9) == 0 ||
	    strncmp(resolved, "/private/var/tmp/", 17) == 0) {
		rmdir(dir);
		GTEST_SKIP() << "HOME is already inside a default temporary root";
	}
	std::string path = std::string(dir) + "/state";
	for (int mode : {0, 1, 2, 3}) {
		int rc = run_in_child([&]() {
			setenv("TMPDIR", mode == 2 ? "/" : mode == 3 ? home : dir, 1);
			struct sandbox_config cfg = {};
			cfg.path_policy_enabled = 1;
			cfg.read_all = 1;
			cfg.allow_temp = mode != 0;
			if (sandbox_enter(&cfg) != 0)
				return 10;
			int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
			if (mode != 1) {
				if (fd >= 0)
					close(fd);
				return fd < 0 ? 0 : 11;
			}
			if (fd < 0)
				return 12;
			close(fd);
			return unlink(path.c_str()) == 0 ? 0 : 13;
		});
		EXPECT_EQ(rc, 0) << "mode=" << mode;
	}
	unlink(path.c_str());
	rmdir(dir);
}

TEST(SandboxToolPathsTest, OverridesListsAndExistingGrants)
{
	char temp[] = "/tmp/morph_tool_overrides_XXXXXX";
	ASSERT_NE(mkdtemp(temp), nullptr);
	std::string base(temp);
	ASSERT_EQ(mkdir((base + "/home").c_str(), 0700), 0);
	ASSERT_EQ(mkdir((base + "/explicit").c_str(), 0700), 0);
	int rc = run_in_child([&]() {
		while (environ && *environ) {
			std::string name(*environ);
			name.resize(name.find('='));
			if (unsetenv(name.c_str()) != 0)
				return 10;
		}
		setenv("HOME", (base + "/home").c_str(), 1);
		setenv("CARGO_HOME", (base + "/cargo").c_str(), 1);
		setenv("RUSTUP_HOME", (base + "/rustup").c_str(), 1);
		setenv("GRADLE_USER_HOME", (base + "/gradle").c_str(), 1);
		setenv("CODEX_HOME", (base + "/codex").c_str(), 1);
		setenv("CLAUDE_CONFIG_DIR", (base + "/claude").c_str(), 1);
		setenv("HF_HOME", (base + "/huggingface").c_str(), 1);
		setenv("OLLAMA_MODELS", (base + "/models").c_str(), 1);
		std::string gopath = base + "/go-one:relative:" + base + "/go-two:/";
		setenv("GOPATH", gopath.c_str(), 1);
		std::string explicit_path = base + "/explicit";
		char *grants[] = {explicit_path.data()};
		struct sandbox_config cfg = {};
		cfg.path_policy_enabled = 1;
		cfg.read_all = 1;
		cfg.write_paths = grants;
		cfg.write_paths_count = 1;
		cfg.delete_paths = grants;
		cfg.delete_paths_count = 1;
		struct arena *arena = arena_create(1024);
		if (!arena || sandbox_prepare_tool_paths(&cfg, arena) != 0)
			return 11;
		int count = cfg.write_paths_count;
		if (sandbox_prepare_tool_paths(&cfg, arena) != 0 ||
		    cfg.write_paths_count != count)
			return 12;
		if (sandbox_enter(&cfg) != 0)
			return 13;
		for (const char *dir : {"cargo", "rustup", "gradle", "go-one",
				       "go-two", "explicit", "codex", "claude",
				       "huggingface", "models"}) {
			std::string first = base + "/" + dir + "/install.tmp";
			std::string second = base + "/" + dir + "/install";
			int fd = open(first.c_str(), O_CREAT | O_WRONLY, 0600);
			if (fd < 0)
				return 14;
			close(fd);
			if (rename(first.c_str(), second.c_str()) != 0 ||
			    unlink(second.c_str()) != 0)
				return 15;
		}
		int fd = open((base + "/unrelated").c_str(), O_CREAT | O_WRONLY, 0600);
		if (fd >= 0) {
			close(fd);
			return 16;
		}
		arena_destroy(arena);
		return 0;
	});
	EXPECT_EQ(rc, 0);
	std::string cleanup = std::string("rm -rf ") + temp;
	EXPECT_EQ(system(cleanup.c_str()), 0);
}

TEST(SandboxToolPathsTest, ApprovalUsesTheSameDeveloperPresets)
{
	const char *real_home = getenv("HOME");
	ASSERT_NE(real_home, nullptr);
	char temp[PATH_MAX];
	ASSERT_LT(snprintf(temp, sizeof(temp), "%s/.morph_approval_XXXXXX", real_home),
		(int)sizeof(temp));
	ASSERT_NE(mkdtemp(temp), nullptr);
	std::string base(temp);
	ASSERT_EQ(mkdir((base + "/home").c_str(), 0700), 0);
	ASSERT_EQ(mkdir((base + "/work").c_str(), 0700), 0);
	int rc = run_in_child([&]() {
		while (environ && *environ) {
			std::string name(*environ);
			name.resize(name.find('='));
			if (unsetenv(name.c_str()) != 0)
				return 10;
		}
		std::string home = base + "/home";
		setenv("HOME", home.c_str(), 1);
		struct tool_context *tctx = tool_context_create(
			(base + "/work").c_str(), (base + "/work").c_str());
		if (!tctx)
			return 11;
		int approvals = 0;
		tool_context_set_operation_approval(tctx,
			[](const struct tool_operation *, void *data) {
				++*static_cast<int *>(data);
				return TOOL_OP_DENY;
			}, &approvals);
		struct tool_registry registry;
		tool_registry_init(&registry);
		if (exec_tool_init(&registry, tctx, nullptr) != 0 ||
		    request_permissions_init(&registry, tctx) != 0)
			return 12;
		if (tctx->builtin_paths.nelts <= TOOL_CONTEXT_ALLOW_MAX)
			return 13;
		const char *roots[] = {".gradle", ".config", ".morph/skills", ".codex"};
		for (const char *root : roots) {
			std::string dir = home + "/" + root;
			std::string file = dir + "/config.txt";
			int fd = open(file.c_str(), O_CREAT | O_WRONLY, 0600);
			if (fd < 0)
				return 14;
			close(fd);
			for (enum tool_path_op op : {TOOL_PATH_READ, TOOL_PATH_WRITE,
						    TOOL_PATH_DELETE}) {
				if (tool_context_authorize_path(tctx, op, file.c_str(),
					NULL, 0) != 0)
					return 15;
			}
			if (tool_context_authorize_path(tctx, TOOL_PATH_LIST,
				dir.c_str(), NULL, 0) != 0 ||
			    tool_context_request_write_access(tctx, "gradle", "gradle test",
				dir.c_str(), NULL, 0) != 0 ||
			    tool_context_request_delete_access(tctx, "gradle", "gradle clean",
				dir.c_str(), NULL, 0) != 0)
				return 16;
			struct tool_operation operation = {};
			operation.kind = TOOL_OP_PATH_WRITE;
			operation.target = file.c_str();
			enum tool_operation_verdict verdict = TOOL_OP_DENY;
			if (tool_context_check_operation_verdict(tctx, &operation,
				&verdict) != 0 || verdict != TOOL_OP_ALLOW)
				return 17;
			std::string request = "{\"command\":\"gradle test\","
				"\"permissions\":{\"file_system\":{\"write\":[\"" + dir +
				"\"],\"delete\":[\"" + dir + "\"]}}}";
			struct tool_result result;
			tool_result_init(&result);
			if (tool_exec(&registry, "request_permissions", request.c_str(),
				&result) != 0 || !result.envelope ||
			    cJSON_GetObjectItem(result.envelope, "error"))
				return 18;
			tool_result_cleanup(&result);
		}
		if (approvals != 0)
			return 19;
		std::string outside = home + "/private";
		if (mkdir(outside.c_str(), 0700) != 0 ||
		    symlink(outside.c_str(), (home + "/.gradle/escape").c_str()) != 0)
			return 20;
		for (const std::string &file : {home + "/.gradle-other/config",
				home + "/.gradle/escape/config", home + "/.morph/config.toml"}) {
			if (tool_context_authorize_path(tctx, TOOL_PATH_WRITE,
				file.c_str(), NULL, 0) != -EACCES)
				return 21;
		}
		if (approvals != 3)
			return 22;
		if (rename((home + "/.gradle").c_str(),
			   (home + "/.gradle-old").c_str()) != 0 ||
		    symlink(outside.c_str(), (home + "/.gradle").c_str()) != 0)
			return 23;
		if (tool_context_authorize_path(tctx, TOOL_PATH_WRITE,
			(home + "/.gradle/config").c_str(), NULL, 0) != -EACCES ||
		    approvals != 4)
			return 24;
		tool_registry_cleanup(&registry);
		tool_context_destroy(tctx);
		return 0;
	});
	EXPECT_EQ(rc, 0);
	std::filesystem::remove_all(temp);
}
