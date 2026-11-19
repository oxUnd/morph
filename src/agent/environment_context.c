#include "environment_context.h"
#include <nb/cleanup.h>
#include "errors.h"
#include <nb/file.h>
#include <nb/str.h>
#include <nb/utf8.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#endif
#if defined(_WIN32) && !defined(__CYGWIN__)
#define ENV_NATIVE_WINDOWS 1
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/utsname.h>
#endif

#define ENV_METADATA_MAX 8192
#define ENV_DATE_CAP 11
#define ENV_OFFSET_CAP 32

static int equal_ascii(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *b) {
		if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++))
			return 0;
	}
	return *a == *b;
}

static const char *normalize_os(const char *value)
{
	static const struct { const char *alias; const char *name; } names[] = {
		{ "darwin", "macOS" }, { "macos", "macOS" },
		{ "linux", "Linux" }, { "linux-gnu", "Linux" },
		{ "windows", "Windows" }, { "win32", "Windows" },
		{ "freebsd", "FreeBSD" }, { "openbsd", "OpenBSD" },
		{ "netbsd", "NetBSD" },
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (equal_ascii(value, names[i].alias))
			return names[i].name;
	}
	return value;
}

static const char *normalize_arch(const char *value)
{
	if (equal_ascii(value, "aarch64") || equal_ascii(value, "arm64"))
		return "arm64";
	if (equal_ascii(value, "amd64") || equal_ascii(value, "x64") ||
	    equal_ascii(value, "x86_64"))
		return "x86_64";
	if (equal_ascii(value, "arm") || equal_ascii(value, "armv7l") ||
	    equal_ascii(value, "armv6l"))
		return "arm";
	if (equal_ascii(value, "riscv64"))
		return "riscv64";
	return value;
}

static const char *shell_name(const char *value)
{
	const char *name = value;

	if (!value)
		return NULL;
	for (const char *p = value; *p; p++) {
		if (*p == '/' || *p == '\\')
			name = p + 1;
	}
	if (equal_ascii(name, "powershell.exe") || equal_ascii(name, "pwsh.exe") ||
	    equal_ascii(name, "pwsh"))
		return "powershell";
	if (equal_ascii(name, "cmd.exe"))
		return "cmd";
	return name;
}

/* Decode with the shared UTF-8 API; drop malformed and XML-illegal data. */
static int xml_field(nb_buf *out, const char *name, const char *value)
{
	size_t start = out->len;
	size_t content_start;
	size_t remaining;

	if (!value || !*value)
		return 0;
	nb_buf_printf(out, "  <%s>", name);
	content_start = out->len;
	remaining = strlen(value);
	while (remaining) {
		unsigned cp;
		size_t bytes;
		const char *escaped = NULL;

		if (!nb_utf8_read(value, remaining, &cp, &bytes)) {
			value++;
			remaining--;
			continue;
		}
		switch (cp) {
		case '&': escaped = "&amp;"; break;
		case '<': escaped = "&lt;"; break;
		case '>': escaped = "&gt;"; break;
		case '"': escaped = "&quot;"; break;
		case '\'': escaped = "&apos;"; break;
		case '\n': escaped = "&#10;"; break;
		case '\r': escaped = "&#13;"; break;
		case '\t': escaped = "&#9;"; break;
		default: break;
		}
		if (escaped)
			nb_buf_append_cstr(out, escaped);
		else if ((cp >= 0x20 && cp <= 0xd7ff) ||
			 (cp >= 0xe000 && cp <= 0xfffd) ||
			 (cp >= 0x10000 && cp <= 0x10ffff))
			nb_buf_append(out, value, bytes);
		value += bytes;
		remaining -= bytes;
	}
	if (nb_buf_status(out))
		return nb_buf_status(out);
	if (out->len == content_start)
		return (nb_buf_truncate(out, start), 0);
	return (nb_buf_printf(out, "</%s>\n", name) ? 0 : NB_ERR_NOMEM);
}

int environment_context_render(const struct environment_context *ctx, nb_buf *out)
{
	int rc;
	if (!ctx || !out)
		return NB_ERR_INVAL;
	const struct { const char *name; const char *value; } fields[] = {
		{ "cwd", ctx->cwd },
		{ "morph_executable", ctx->morph_executable },
		{ "shell", shell_name(ctx->shell) },
		{ "os", normalize_os(ctx->os) },
		{ "os_version", ctx->os_version },
		{ "distribution", ctx->distribution },
		{ "kernel_release", ctx->kernel_release },
		{ "arch", normalize_arch(ctx->arch) },
		{ "runtime", ctx->runtime },
		{ "runtime_version", ctx->runtime_version },
		{ "host_os", ctx->host_os },
		{ "path_tools", ctx->path_tools },
		{ "current_date", ctx->current_date },
		{ "timezone", ctx->timezone },
		{ "git_branch", ctx->git_branch },
		{ "git_root", ctx->git_root },
		{ "sandbox", ctx->sandbox },
	};

	rc = (nb_buf_append_cstr(out, "<environment_context>\n") ? 0 : NB_ERR_NOMEM);
	for (size_t i = 0; rc == 0 && i < sizeof(fields) / sizeof(fields[0]); i++)
		rc = xml_field(out, fields[i].name, fields[i].value);
	if (rc != 0)
		return rc;
	return (nb_buf_append_cstr(out, "</environment_context>\n") ? 0 : NB_ERR_NOMEM);
}

struct environment_collector {
	int error;
};

#ifdef ENV_NATIVE_WINDOWS
static void *collect_alloc(struct environment_collector *collector, size_t size)
{
	void *result = calloc(1, size);

	if (!result)
		collector->error = (NB_ERR_NOMEM);
	return result;
}
#endif

static char *copy_value(struct environment_collector *collector, const char *value)
{
	char *copy;

	if (!value || !*value)
		return NULL;
	copy = strdup(value);
	if (!copy)
		collector->error = (NB_ERR_NOMEM);
	return copy;
}

enum environment_wsl environment_wsl_detect(nb_str release, bool have_interop)
{
	nb_split_iter parts;
	nb_str part;
	bool previous_microsoft = false;

	nb_split_init(&parts, release, '-');
	while (nb_split_next(&parts, &part)) {
		if (nb_str_eq_icase(part, NB_STR_LIT("WSL2")) ||
		    (previous_microsoft && nb_str_eq_icase(part, NB_STR_LIT("standard"))))
			return ENV_WSL_2;
		previous_microsoft = nb_str_eq_icase(part, NB_STR_LIT("Microsoft"));
	}
	if (previous_microsoft && nb_str_starts_with(release, NB_STR_LIT("4.4.0-")))
		return ENV_WSL_1;
	return previous_microsoft || have_interop ? ENV_WSL_UNKNOWN : ENV_WSL_NONE;
}

#if defined(_WIN32) || defined(__CYGWIN__)
static char *windows_version(struct environment_collector *collector)
{
	/* RtlGetVersion reports the actual build independently of the app manifest. */
	typedef LONG (WINAPI *windows_version_fn)(OSVERSIONINFOW *);
	HMODULE module = GetModuleHandleW(L"ntdll.dll");
	FARPROC symbol = module ? GetProcAddress(module, "RtlGetVersion") : NULL;
	OSVERSIONINFOW info = {.dwOSVersionInfoSize = sizeof(info)};
	char version[64];

	if (!symbol)
		return NULL;
	windows_version_fn get_version = (windows_version_fn)(void (*)(void))symbol;
	if (get_version(&info) != 0)
		return NULL;
	snprintf(version, sizeof(version), "%lu.%lu.%lu", (unsigned long)info.dwMajorVersion,
		 (unsigned long)info.dwMinorVersion, (unsigned long)info.dwBuildNumber);
	return copy_value(collector, version);
}
#endif

#ifdef ENV_NATIVE_WINDOWS
static void wide_cleanup(wchar_t **value)
{
	free(*value);
	*value = NULL;
}

static wchar_t *wide_path(struct environment_collector *collector, const char *path)
{
	int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		path, -1, NULL, 0);
	wchar_t *wide;

	if (!count)
		return NULL;
	wide = collect_alloc(collector, (size_t)count * sizeof(*wide));
	if (wide)
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			path, -1, wide, count);
	return wide;
}

static char *full_path(struct environment_collector *collector, const char *path)
{
	wchar_t *input NB_CLEANUP(wide_cleanup) = wide_path(collector, path && *path ? path : ".");
	DWORD count;
	DWORD written;
	wchar_t *wide NB_CLEANUP(wide_cleanup) = NULL;
	char *utf8;
	int bytes;

	if (!input)
		return NULL;
	count = GetFullPathNameW(input, 0, NULL, NULL);
	if (!count)
		return NULL;
	wide = collect_alloc(collector, (size_t)count * sizeof(*wide));
	if (!wide)
		return NULL;
	written = GetFullPathNameW(input, count, wide, NULL);
	if (!written || written >= count)
		return NULL;
	bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
	if (!bytes)
		return NULL;
	utf8 = collect_alloc(collector, (size_t)bytes);
	if (utf8)
		WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, bytes, NULL, NULL);
	return utf8;
}
#else
static char *full_path(struct environment_collector *collector, const char *path)
{
	char cwd[PATH_MAX];

	if (path && *path) {
		nb_buf canonical NB_CLEANUP(nb_buf_destroy) = {0};
		if (nb_path_canonical(&canonical, path, NULL) != NB_OK)
			return NULL;
		return copy_value(collector, nb_buf_cstr(&canonical));
	} else if (!getcwd(cwd, sizeof(cwd))) {
		return NULL;
	}
	return copy_value(collector, cwd);
}
#endif

static char *join_path(struct environment_collector *collector, const char *base, const char *name)
{
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	if (!base)
		return NULL;
	rc = (nb_buf_init(&buf), nb_buf_reserve(&buf, 128) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_printf(&buf, "%s/%s", base, name) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		collector->error = (rc);
		return NULL;
	}
	return (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
}

/* 0: absent, 1: file, 2: directory, -1: unavailable or unsupported. */
static int path_kind(struct environment_collector *collector, const char *path)
{
	if (!path)
		return -1;
#ifdef ENV_NATIVE_WINDOWS
	wchar_t *wide NB_CLEANUP(wide_cleanup) = wide_path(collector, path);
	DWORD attributes = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;

	if (attributes == INVALID_FILE_ATTRIBUTES)
		return 0;
	return attributes & FILE_ATTRIBUTE_DIRECTORY ? 2 : 1;
#else
	struct stat info;
	(void)collector;
	if (stat(path, &info) != 0)
		return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
	if (S_ISDIR(info.st_mode))
		return 2;
	return S_ISREG(info.st_mode) ? 1 : -1;
#endif
}

/* Bounded regular-file reads only: never block on a FIFO in a workspace. */
static char *read_metadata(struct environment_collector *collector, const char *path)
{
	char data[ENV_METADATA_MAX + 1];
	size_t len;
#ifdef ENV_NATIVE_WINDOWS
	if (path_kind(collector, path) != 1)
		return NULL;
	wchar_t *wide NB_CLEANUP(wide_cleanup) = wide_path(collector, path);
	FILE *file = wide ? _wfopen(wide, L"rb") : NULL;
	struct _stat info;

	if (!file)
		return NULL;
	if (_fstat(_fileno(file), &info) || !(info.st_mode & _S_IFREG)) {
		fclose(file);
		return NULL;
	}
	len = fread(data, 1, sizeof(data), file);
	int failed = ferror(file);
	fclose(file);
	if (failed)
		return NULL;
#else
	struct stat info;
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		return NULL;
	if (fstat(fd, &info) || !S_ISREG(info.st_mode)) {
		close(fd);
		return NULL;
	}
	len = 0;
	while (len < sizeof(data)) {
		ssize_t result = read(fd, data + len, sizeof(data) - len);

		if (result < 0 && errno == EINTR)
			continue;
		if (result <= 0) {
			if (result < 0)
				len = 0;
			break;
		}
		len += (size_t)result;
	}
	close(fd);
#endif
	if (!len || len > ENV_METADATA_MAX || memchr(data, '\0', len))
		return NULL;
	while (len && (data[len - 1] == '\n' || data[len - 1] == '\r'))
		len--;
	data[len] = '\0';
	return copy_value(collector, data);
}

#if defined(__linux__)
static char *os_release_value(struct environment_collector *collector,
			      const char *metadata, nb_str key)
{
	nb_split_iter lines;
	nb_str line;
	nb_str value = NB_STR_NULL;
	nb_buf decoded NB_CLEANUP(nb_buf_cleanup) = {0};

	nb_split_init(&lines, nb_str_from_cstr(metadata), '\n');
	while (nb_split_next(&lines, &line)) {
		line = nb_str_trim(line);
		if (line.len > key.len && line.ptr[key.len] == '=' &&
		    nb_str_starts_with(line, key))
			value = nb_str_trim(nb_str_from_parts(line.ptr + key.len + 1,
							    line.len - key.len - 1));
	}
	if (!value.len)
		return NULL;

	char quote = '\0';
	if (value.ptr[0] == '"' || value.ptr[0] == '\'') {
		quote = value.ptr[0];
		if (value.len < 2 || value.ptr[value.len - 1] != quote)
			return NULL;
		value.ptr++;
		value.len -= 2;
	}
	for (size_t i = 0; i < value.len; i++) {
		char c = value.ptr[i];

		if (c == quote)
			return NULL;
		if (c == '\\' && quote != '\'') {
			if (i + 1 == value.len)
				return NULL;
			if (!quote || strchr("\\\"$\x60", value.ptr[i + 1]))
				c = value.ptr[++i];
		}
		if (!nb_buf_append_char(&decoded, c)) {
			collector->error = nb_buf_status(&decoded);
			return NULL;
		}
	}
	return copy_value(collector, nb_buf_cstr(&decoded));
}

static void collect_linux_distribution(struct environment_context *out,
				       struct environment_collector *collector)
{
	const char *primary = "/etc/os-release";
	char *metadata NB_CLEANUP(nb_charp_cleanup) = read_metadata(collector, primary);

	if (!metadata && path_kind(collector, primary) == 0)
		metadata = read_metadata(collector, "/usr/lib/os-release");
	if (!metadata)
		return;
	out->distribution = os_release_value(collector, metadata, NB_STR_LIT("NAME"));
	if (!out->distribution)
		out->distribution = os_release_value(collector, metadata, NB_STR_LIT("ID"));
	out->os_version = os_release_value(collector, metadata, NB_STR_LIT("VERSION_ID"));
	if (!out->os_version)
		out->os_version = os_release_value(collector, metadata, NB_STR_LIT("VERSION"));
}

static bool have_wsl_interop(void)
{
	nb_str path = nb_str_from_cstr(getenv("WSL_INTEROP"));
	struct stat info;

	return stat("/proc/sys/fs/binfmt_misc/WSLInterop", &info) == 0 ||
	       (nb_str_starts_with(path, NB_STR_LIT("/run/WSL/")) &&
		stat(path.ptr, &info) == 0 && S_ISSOCK(info.st_mode));
}
#endif

static void collect_platform(struct environment_context *out,
			     struct environment_collector *collector)
{
#if defined(_WIN32) || defined(__CYGWIN__)
	out->os = copy_value(collector, "Windows");
	out->os_version = windows_version(collector);
	out->kernel_release = copy_value(collector, out->os_version);
#endif
#ifdef ENV_NATIVE_WINDOWS
	SYSTEM_INFO info;
	const char *arch = NULL;

	GetNativeSystemInfo(&info);
	switch (info.wProcessorArchitecture) {
	case PROCESSOR_ARCHITECTURE_AMD64: arch = "x86_64"; break;
	case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
	case PROCESSOR_ARCHITECTURE_ARM: arch = "arm"; break;
	case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
	default: break;
	}
	out->arch = copy_value(collector, arch);
#else
	struct utsname info;
#if defined(__CYGWIN__)
	out->runtime = copy_value(collector, "Cygwin");
#endif
	if (uname(&info) != 0)
		return;
	out->arch = copy_value(collector, normalize_arch(info.machine));
#if defined(__CYGWIN__)
	out->runtime_version = copy_value(collector, info.release);
#else
	out->os = copy_value(collector, normalize_os(info.sysname));
	out->kernel_release = copy_value(collector, info.release);
#if defined(__APPLE__)
	char version[64] = {0};
	size_t size = sizeof(version);

	if (sysctlbyname("kern.osproductversion", version, &size, NULL, 0) == 0 &&
	    size && size <= sizeof(version) && version[size - 1] == '\0')
		out->os_version = copy_value(collector, version);
#elif defined(__linux__)
	collect_linux_distribution(out, collector);
	nb_str release = nb_str_from_cstr(info.release);
	enum environment_wsl runtime = environment_wsl_detect(release, have_wsl_interop());

	if (runtime != ENV_WSL_NONE) {
		const char *name = "WSL";

		if (runtime == ENV_WSL_1)
			name = "WSL1";
		else if (runtime == ENV_WSL_2)
			name = "WSL2";
		out->runtime = copy_value(collector, name);
		out->host_os = copy_value(collector, "Windows");
	}
#endif
#endif
#endif
}

static void collect_path_tools(struct environment_context *out,
			       struct environment_collector *collector)
{
	nb_buf found NB_CLEANUP(nb_buf_cleanup) = {0};
#ifdef ENV_NATIVE_WINDOWS
	const char *const names[] = {
		"rg.exe", "fd.exe", "where.exe", "findstr.exe", "cat.exe", "more.com",
		"sed.exe", "head.exe", "tail.exe", "tee.exe", "patch.exe", "fc.exe",
		"robocopy.exe", "git.exe",
	};
	DWORD size = GetEnvironmentVariableW(L"PATH", NULL, 0);
	wchar_t *path NB_CLEANUP(wide_cleanup) = NULL;

	if (!size)
		return;
	path = collect_alloc(collector, (size_t)size * sizeof(*path));
	if (!path)
		return;
	DWORD written = GetEnvironmentVariableW(L"PATH", path, size);
	if (!written || written >= size)
		return;
#else
	const char *const names[] = {
		"rg", "fd", "find", "grep", "cat", "sed", "head", "tail", "tee", "patch", "git",
	};
	const char *path = getenv("PATH");

	if (!path)
		return;
#endif
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
#ifdef ENV_NATIVE_WINDOWS
		wchar_t name[32];
		wchar_t candidate[PATH_MAX];
		DWORD len;

		if (!MultiByteToWideChar(CP_UTF8, 0, names[i], -1, name,
					 (int)(sizeof(name) / sizeof(name[0]))))
			continue;
		len = SearchPathW(path, name, NULL,
				  (DWORD)(sizeof(candidate) / sizeof(candidate[0])),
				  candidate, NULL);
		if (!len || len >= sizeof(candidate) / sizeof(candidate[0]))
			continue;
		DWORD attrs = GetFileAttributesW(candidate);
		if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
			continue;
#else
		nb_split_iter dirs;
		nb_str dir;
		bool executable = false;

		nb_split_init(&dirs, nb_str_from_cstr(path), ':');
		while (nb_split_next(&dirs, &dir)) {
			char candidate[PATH_MAX];
			struct stat info;
			bool absolute = dir.len && dir.ptr[0] == '/';

			if (dir.len >= sizeof(candidate) || (!absolute && !out->cwd))
				continue;
			int len = absolute ?
				snprintf(candidate, sizeof(candidate), "%.*s/%s",
					 (int)dir.len, dir.ptr, names[i]) :
				snprintf(candidate, sizeof(candidate), "%s/%.*s/%s",
					 out->cwd, (int)dir.len, dir.ptr, names[i]);
			if (len > 0 && (size_t)len < sizeof(candidate) &&
			    stat(candidate, &info) == 0 && S_ISREG(info.st_mode) &&
			    access(candidate, X_OK) == 0) {
				executable = true;
				break;
			}
		}
		if (!executable)
			continue;
#endif
		if (!nb_buf_printf(&found, "%s%s", found.len ? ", " : "", names[i])) {
			collector->error = nb_buf_status(&found);
			return;
		}
	}
	out->path_tools = copy_value(collector, nb_buf_cstr(&found));
}

static int is_absolute(const char *path)
{
#ifdef ENV_NATIVE_WINDOWS
	return path[0] == '/' || path[0] == '\\' ||
		(isalpha((unsigned char)path[0]) && path[1] == ':');
#else
	return path[0] == '/';
#endif
}

static void collect_git(struct environment_context *out, struct environment_collector *collector)
{
	char *dir NB_CLEANUP(nb_charp_cleanup) = NULL;
#ifndef ENV_NATIVE_WINDOWS
	struct stat origin;
#endif

	/* Overrides require Git's own resolver; omit rather than guess. */
	if (!out->cwd || getenv("GIT_DIR") || getenv("GIT_WORK_TREE") ||
	    getenv("GIT_CEILING_DIRECTORIES") || getenv("GIT_COMMON_DIR") ||
	    getenv("GIT_DISCOVERY_ACROSS_FILESYSTEM"))
		return;
#ifndef ENV_NATIVE_WINDOWS
	if (stat(out->cwd, &origin) != 0)
		return;
#endif
	dir = copy_value(collector, out->cwd);
	while (dir && *dir) {
		char *dotgit NB_CLEANUP(nb_charp_cleanup) = join_path(collector, dir, ".git");
		char *link NB_CLEANUP(nb_charp_cleanup) = NULL;
		char *resolved NB_CLEANUP(nb_charp_cleanup) = NULL;
		char *head_path NB_CLEANUP(nb_charp_cleanup) = NULL;
		char *head NB_CLEANUP(nb_charp_cleanup) = NULL;
		const char *gitdir = dotgit;
		char *slash;
		int kind;
#ifndef ENV_NATIVE_WINDOWS
		struct stat current;
		if (stat(dir, &current) != 0 || current.st_dev != origin.st_dev)
			return;
#endif
		if (!dotgit)
			return;
		kind = path_kind(collector, dotgit);
		link = read_metadata(collector, dotgit);
		if (link && !strncmp(link, "gitdir: ", 8)) {
			if (is_absolute(link + 8))
				gitdir = link + 8;
			else {
				resolved = join_path(collector, dir, link + 8);
				gitdir = resolved;
			}
		}
		head_path = join_path(collector, gitdir, "HEAD");
		if (!head_path)
			return;
		head = read_metadata(collector, head_path);
		if (head && !strchr(head, '\n') && !strchr(head, '\r')) {
			char *common_path NB_CLEANUP(nb_charp_cleanup) =
				join_path(collector, gitdir, "commondir");
			char *common NB_CLEANUP(nb_charp_cleanup) =
				common_path ? read_metadata(collector, common_path) : NULL;
			char *common_base NB_CLEANUP(nb_charp_cleanup) =
				common && !is_absolute(common)
					? join_path(collector, gitdir, common)
					: NULL;
			const char *base =
				common ? (is_absolute(common) ? common : common_base) : gitdir;
			char *objects NB_CLEANUP(nb_charp_cleanup) =
				join_path(collector, base, "objects");
			char *refs NB_CLEANUP(nb_charp_cleanup) =
				join_path(collector, base, "refs");

			if (path_kind(collector, objects) != 2 || path_kind(collector, refs) != 2)
				return;
			if (!strncmp(head, "ref: refs/heads/", 16) && head[16]) {
				out->git_branch = copy_value(collector, head + 16);
				out->git_root = copy_value(collector, dir);
				return;
			}
			if ((strlen(head) == 40 || strlen(head) == 64) &&
			    strspn(head, "0123456789abcdefABCDEF") == strlen(head)) {
				out->git_branch = copy_value(collector, "detached");
				out->git_root = copy_value(collector, dir);
				return;
			}
		}
		if (kind != 0)
			return;
		slash = strrchr(dir, '/');
#ifdef ENV_NATIVE_WINDOWS
		char *backslash = strrchr(dir, '\\');
		if (!slash || (backslash && backslash > slash))
			slash = backslash;
#endif
		if (!slash || !slash[1])
			break;
		if (slash == dir)
			slash[1] = '\0';
		else
			*slash = '\0';
	}
}

#ifndef ENV_NATIVE_WINDOWS
static int iana_name(const char *name)
{
	if (!name || !*name || *name == '/' || strstr(name, ".."))
		return 0;
	if (!strcmp(name, "UTC") || !strcmp(name, "GMT"))
		return 1;
	if (!strchr(name, '/'))
		return 0;
	for (const char *p = name; *p; p++) {
		if (!isalnum((unsigned char)*p) && !strchr("/_-+", *p))
			return 0;
	}
	return 1;
}

#endif

static char *timezone_identity(struct environment_collector *collector)
{
	const char *tz = getenv("TZ");
	const char *name;

	if (tz) {
		if (*tz == ':')
			tz++;
		name = strstr(tz, "/zoneinfo/");
		name = name ? name + strlen("/zoneinfo/") : tz;
#ifndef ENV_NATIVE_WINDOWS
		char *zone_path NB_CLEANUP(nb_charp_cleanup) =
			join_path(collector, "/usr/share/zoneinfo", name);
		if (iana_name(name) && path_kind(collector, zone_path) == 1)
			return copy_value(collector, name);
#else
		if (!strcmp(name, "UTC") || !strcmp(name, "GMT"))
			return copy_value(collector, name);
#endif
		return NULL;
	}
#ifndef ENV_NATIVE_WINDOWS
	char path[PATH_MAX];
	ssize_t len = readlink("/etc/localtime", path, sizeof(path) - 1);

	if (len > 0 && (size_t)len < sizeof(path) - 1) {
		path[len] = '\0';
		name = strstr(path, "/zoneinfo/");
		if (name && iana_name(name + strlen("/zoneinfo/")))
			return copy_value(collector, name + strlen("/zoneinfo/"));
	}
	/* A copied localtime file has no reliably recoverable IANA identity. */
#endif
	return NULL;
}

int environment_context_create(struct environment_context *out,
			       const struct prompt_context_input *input)
{
	struct environment_collector state = {0};
	struct environment_collector *collector = &state;
	nb_buf executable NB_CLEANUP(nb_buf_cleanup) = {0};
	time_t now;
	struct tm local;
	char date[ENV_DATE_CAP];
	char offset[ENV_OFFSET_CAP];
	int have_local;

	if (!out)
		return NB_ERR_INVAL;

	if (out->cwd || out->morph_executable || out->shell || out->os ||
	    out->os_version || out->distribution ||
	    out->kernel_release || out->arch || out->runtime || out->runtime_version ||
	    out->host_os || out->path_tools || out->current_date || out->timezone ||
	    out->git_branch || out->git_root || out->sandbox)
		return NB_ERR_STATE;

	memset(out, 0, sizeof(*out));
	nb_status status = nb_path_executable(&executable, NULL);
	if (status == NB_ERR_NOMEM)
		return status;

	if (status == NB_OK)
		out->morph_executable = copy_value(collector, nb_buf_cstr(&executable));

	out->cwd = full_path(collector, input ? input->cwd : NULL);
	if (out->cwd && path_kind(collector, out->cwd) != 2) {
		free((void *)out->cwd);
		out->cwd = NULL;
	}
	out->shell = copy_value(collector, input && input->shell && *input->shell ?
		input->shell : getenv("SHELL"));
#ifdef ENV_NATIVE_WINDOWS
	if (!out->shell)
		out->shell = copy_value(collector, getenv("COMSPEC"));
#endif
	out->sandbox = copy_value(collector, input ? input->sandbox : NULL);
	collect_platform(out, collector);
	collect_path_tools(out, collector);
	now = time(NULL);
#ifdef ENV_NATIVE_WINDOWS
	have_local = now != (time_t)-1 && localtime_s(&local, &now) == 0;
#else
	have_local = now != (time_t)-1 && localtime_r(&now, &local) != NULL;
#endif
	if (have_local && strftime(date, sizeof(date), "%Y-%m-%d", &local))
		out->current_date = copy_value(collector, date);
	out->timezone = timezone_identity(collector);
	if (!out->timezone && have_local &&
	    strftime(offset, sizeof(offset), "%z", &local) == 5) {
		nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};
		int rc = (nb_buf_init(&buf),
			  nb_buf_reserve(&buf, ENV_OFFSET_CAP) ? 0 : NB_ERR_NOMEM);
		if (rc == 0)
			rc = (nb_buf_printf(&buf, "UTC%.3s:%.2s", offset, offset + 3)
				      ? 0
				      : NB_ERR_NOMEM);
		if (rc != 0) {
			environment_context_cleanup(out);
			return rc;
		}
		out->timezone = (nb_buf_ok(&buf) ? strdup(nb_buf_cstr(&buf)) : NULL);
	}
	collect_git(out, collector);
	if (collector->error) {
		environment_context_cleanup(out);
		return collector->error;
	}
	return 0;
}

void environment_context_cleanup(struct environment_context *ctx)
{
	if (!ctx)
		return;
	free((void *)ctx->cwd);
	free((void *)ctx->morph_executable);
	free((void *)ctx->shell);
	free((void *)ctx->os);
	free((void *)ctx->os_version);
	free((void *)ctx->distribution);
	free((void *)ctx->kernel_release);
	free((void *)ctx->arch);
	free((void *)ctx->runtime);
	free((void *)ctx->runtime_version);
	free((void *)ctx->host_os);
	free((void *)ctx->path_tools);
	free((void *)ctx->current_date);
	free((void *)ctx->timezone);
	free((void *)ctx->git_branch);
	free((void *)ctx->git_root);
	free((void *)ctx->sandbox);
	memset(ctx, 0, sizeof(*ctx));
}
