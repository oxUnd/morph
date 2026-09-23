#include "environment_context.h"
#include "util/error.h"
#include "util/utf8.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
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
static int xml_field(morph_buf_t *out, const char *name, const char *value)
{
	size_t start = out->len;
	size_t content_start;
	size_t remaining;

	if (!value || !*value)
		return 0;
	morph_buf_printf(out, "  <%s>", name);
	content_start = out->len;
	remaining = strlen(value);
	while (remaining) {
		unsigned cp;
		size_t bytes;
		const char *escaped = NULL;

		if (!utf8_decode_codepoint(value, remaining, &cp, &bytes)) {
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
			morph_buf_puts(out, escaped);
		else if ((cp >= 0x20 && cp <= 0xd7ff) ||
			 (cp >= 0xe000 && cp <= 0xfffd) ||
			 (cp >= 0x10000 && cp <= 0x10ffff))
			morph_buf_append(out, value, bytes);
		value += bytes;
		remaining -= bytes;
	}
	if (out->failed)
		MORPH_RETURN(out->failed);
	if (out->len == content_start)
		return morph_buf_truncate(out, start);
	return morph_buf_printf(out, "</%s>\n", name);
}

int environment_context_render(const struct environment_context *ctx,
	morph_buf_t *out)
{
	int rc;
	if (!ctx || !out)
		MORPH_RETURN(-EINVAL);
	const struct { const char *name; const char *value; } fields[] = {
		{ "cwd", ctx->cwd },
		{ "shell", shell_name(ctx->shell) },
		{ "os", normalize_os(ctx->os) },
		{ "arch", normalize_arch(ctx->arch) },
		{ "current_date", ctx->current_date },
		{ "timezone", ctx->timezone },
		{ "git_branch", ctx->git_branch },
		{ "git_root", ctx->git_root },
		{ "sandbox", ctx->sandbox },
	};

	rc = morph_buf_puts(out, "<environment_context>\n");
	for (size_t i = 0; rc == 0 && i < sizeof(fields) / sizeof(fields[0]); i++)
		rc = xml_field(out, fields[i].name, fields[i].value);
	if (rc != 0)
		return rc;
	return morph_buf_puts(out, "</environment_context>\n");
}

struct environment_collector {
	struct arena *arena;
	int error;
};

#ifdef _WIN32
static void *collect_alloc(struct environment_collector *collector, size_t size)
{
	void *result = arena_alloc(collector->arena, size);

	if (!result)
		MORPH_SET_ERR(collector->error, -ENOMEM);
	return result;
}
#endif

static const char *copy_value(struct environment_collector *collector, const char *value)
{
	char *copy;

	if (!value || !*value)
		return NULL;
	copy = arena_strdup(collector->arena, value);
	if (!copy)
		MORPH_SET_ERR(collector->error, -ENOMEM);
	return copy;
}

#ifdef _WIN32
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

static const char *full_path(struct environment_collector *collector, const char *path)
{
	wchar_t *input = wide_path(collector, path && *path ? path : ".");
	DWORD count;
	DWORD written;
	wchar_t *wide;
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
static const char *full_path(struct environment_collector *collector, const char *path)
{
	char cwd[PATH_MAX];

	if (path && *path) {
		if (!realpath(path, cwd))
			return NULL;
	} else if (!getcwd(cwd, sizeof(cwd))) {
		return NULL;
	}
	return copy_value(collector, cwd);
}
#endif

static const char *join_path(struct environment_collector *collector, const char *base,
	const char *name)
{
	morph_buf_t buf;
	int rc;

	if (!base)
		return NULL;
	rc = morph_buf_init_arena(&buf, collector->arena, 128);
	if (rc == 0)
		rc = morph_buf_printf(&buf, "%s/%s", base, name);
	if (rc != 0) {
		MORPH_SET_ERR(collector->error, rc);
		return NULL;
	}
	return morph_buf_cstr(&buf);
}

/* 0: absent, 1: file, 2: directory, -1: unavailable or unsupported. */
static int path_kind(struct environment_collector *collector, const char *path)
{
	if (!path)
		return -1;
#ifdef _WIN32
	wchar_t *wide = wide_path(collector, path);
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
static const char *read_metadata(struct environment_collector *collector, const char *path)
{
	char data[ENV_METADATA_MAX + 1];
	size_t len;
#ifdef _WIN32
	if (path_kind(collector, path) != 1)
		return NULL;
	wchar_t *wide = wide_path(collector, path);
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
	ssize_t result;

	if (fd < 0)
		return NULL;
	if (fstat(fd, &info) || !S_ISREG(info.st_mode)) {
		close(fd);
		return NULL;
	}
	result = read(fd, data, sizeof(data));
	close(fd);
	if (result < 0)
		return NULL;
	len = (size_t)result;
#endif
	if (!len || len > ENV_METADATA_MAX || memchr(data, '\0', len))
		return NULL;
	while (len && (data[len - 1] == '\n' || data[len - 1] == '\r'))
		len--;
	data[len] = '\0';
	return copy_value(collector, data);
}

static int is_absolute(const char *path)
{
#ifdef _WIN32
	return path[0] == '/' || path[0] == '\\' ||
		(isalpha((unsigned char)path[0]) && path[1] == ':');
#else
	return path[0] == '/';
#endif
}

static void collect_git(struct environment_context *out, struct environment_collector *collector)
{
	char *dir;
#ifndef _WIN32
	struct stat origin;
#endif

	/* Overrides require Git's own resolver; omit rather than guess. */
	if (!out->cwd || getenv("GIT_DIR") || getenv("GIT_WORK_TREE") ||
	    getenv("GIT_CEILING_DIRECTORIES") ||
	    getenv("GIT_COMMON_DIR") ||
	    getenv("GIT_DISCOVERY_ACROSS_FILESYSTEM"))
		return;
#ifndef _WIN32
	if (stat(out->cwd, &origin) != 0)
		return;
#endif
	dir = (char *)copy_value(collector, out->cwd);
	while (dir && *dir) {
		const char *dotgit = join_path(collector, dir, ".git");
		const char *gitdir = dotgit;
		const char *link;
		const char *head_path;
		const char *head;
		char *slash;
		int kind;

#ifndef _WIN32
		struct stat current;
		if (stat(dir, &current) != 0 || current.st_dev != origin.st_dev)
			return;
#endif
		if (!dotgit)
			return;
		kind = path_kind(collector, dotgit);
		link = read_metadata(collector, dotgit);
		if (link && !strncmp(link, "gitdir: ", 8)) {
			gitdir = is_absolute(link + 8) ? link + 8 :
				join_path(collector, dir, link + 8);
		}
		head_path = join_path(collector, gitdir, "HEAD");
		if (!head_path)
			return;
		head = read_metadata(collector, head_path);
		if (head && !strchr(head, '\n') && !strchr(head, '\r')) {
			const char *common_path = join_path(collector, gitdir, "commondir");
			const char *common = common_path ?
				read_metadata(collector, common_path) : NULL;
			const char *base = common ? (is_absolute(common) ? common :
				join_path(collector, gitdir, common)) : gitdir;

			if (path_kind(collector, join_path(collector, base, "objects")) != 2 ||
			    path_kind(collector, join_path(collector, base, "refs")) != 2)
				return;
			if (!strncmp(head, "ref: refs/heads/", 16) && head[16]) {
				out->git_branch = copy_value(collector, head + 16);
				out->git_root = copy_value(collector, dir);
				return;
			}
			if ((strlen(head) == 40 || strlen(head) == 64) &&
			    strspn(head, "0123456789abcdefABCDEF") == strlen(head)) {
				out->git_branch = "detached";
				out->git_root = copy_value(collector, dir);
				return;
			}
		}
		if (kind != 0)
			return;
		slash = strrchr(dir, '/');
#ifdef _WIN32
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

#ifndef _WIN32
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

static const char *timezone_identity(struct environment_collector *collector)
{
	const char *tz = getenv("TZ");
	const char *name;

	if (tz) {
		if (*tz == ':')
			tz++;
		name = strstr(tz, "/zoneinfo/");
		name = name ? name + strlen("/zoneinfo/") : tz;
#ifndef _WIN32
		const char *zone_path = join_path(collector, "/usr/share/zoneinfo", name);
		if (iana_name(name) && path_kind(collector, zone_path) == 1)
			return copy_value(collector, name);
#else
		if (!strcmp(name, "UTC") || !strcmp(name, "GMT"))
			return copy_value(collector, name);
#endif
		return NULL;
	}
#ifndef _WIN32
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

int environment_context_collect(struct environment_context *out,
	const struct prompt_context_input *input, struct arena *arena)
{
	struct environment_collector state = { .arena = arena };
	struct environment_collector *collector = &state;
	time_t now;
	struct tm local;
	char date[ENV_DATE_CAP];
	char offset[ENV_OFFSET_CAP];
	int have_local;

	if (!out || !arena)
		MORPH_RETURN(-EINVAL);
	memset(out, 0, sizeof(*out));
	out->cwd = full_path(collector, input ? input->cwd : NULL);
	if (out->cwd && path_kind(collector, out->cwd) != 2)
		out->cwd = NULL;
	out->shell = copy_value(collector, input && input->shell && *input->shell ?
		input->shell : getenv("SHELL"));
#ifdef _WIN32
	if (!out->shell)
		out->shell = copy_value(collector, getenv("COMSPEC"));
#endif
	out->sandbox = copy_value(collector, input ? input->sandbox : NULL);
#ifdef _WIN32
	SYSTEM_INFO info;
	GetNativeSystemInfo(&info);
	out->os = "Windows";
	switch (info.wProcessorArchitecture) {
	case PROCESSOR_ARCHITECTURE_AMD64: out->arch = "x86_64"; break;
	case PROCESSOR_ARCHITECTURE_ARM64: out->arch = "arm64"; break;
	case PROCESSOR_ARCHITECTURE_ARM: out->arch = "arm"; break;
	case PROCESSOR_ARCHITECTURE_INTEL: out->arch = "x86"; break;
	default: break;
	}
#else
	struct utsname info;
	if (uname(&info) == 0) {
		out->os = copy_value(collector, normalize_os(info.sysname));
		out->arch = copy_value(collector, normalize_arch(info.machine));
	}
#endif
	now = time(NULL);
#ifdef _WIN32
	have_local = now != (time_t)-1 && localtime_s(&local, &now) == 0;
#else
	have_local = now != (time_t)-1 && localtime_r(&now, &local) != NULL;
#endif
	if (have_local && strftime(date, sizeof(date), "%Y-%m-%d", &local))
		out->current_date = copy_value(collector, date);
	out->timezone = timezone_identity(collector);
	if (!out->timezone && have_local &&
	    strftime(offset, sizeof(offset), "%z", &local) == 5) {
		morph_buf_t buf;
		int rc = morph_buf_init_arena(&buf, arena, ENV_OFFSET_CAP);
		if (rc == 0)
			rc = morph_buf_printf(&buf, "UTC%.3s:%.2s", offset, offset + 3);
		if (rc != 0)
			MORPH_RETURN(rc);
		out->timezone = morph_buf_cstr(&buf);
	}
	collect_git(out, collector);
	if (collector->error)
		MORPH_RETURN(collector->error);
	return 0;
}
