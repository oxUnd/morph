#include "tool_paths.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"
#include "nb/file.h"
#include "nb/log.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Shared developer directories and tool-specific paths for exec policies. */
enum path_base {
	PATH_HOME,
	PATH_ABSOLUTE,
	PATH_CACHE,
	PATH_LIST,
};

enum path_kind {
	PATH_DIRECTORY,
	PATH_EXISTING_DIRECTORY,
	PATH_DEVICE,
};

struct tool_path {
	const char *tool;
	enum path_base base;
	const char *suffix;
	const char *env;
	enum path_kind kind;
};

#define TOOL_DIR(tool, base, suffix, env) \
	{tool, base, suffix, env, PATH_DIRECTORY}
#define HOME_DIR(tool, suffix, env) TOOL_DIR(tool, PATH_HOME, suffix, env)
#define ENV_DIR(tool, env) TOOL_DIR(tool, PATH_ABSOLUTE, "", env)

static const struct tool_path tool_paths[] = {
	/* Shared roots remain available to tools without an individual entry. */
	TOOL_DIR("xdg-cache", PATH_HOME, ".cache", NULL),
	TOOL_DIR("xdg-cache", PATH_HOME, ".cache", "XDG_CACHE_HOME"),
	TOOL_DIR("xdg-config", PATH_HOME, ".config", NULL),
	TOOL_DIR("xdg-config", PATH_HOME, ".config", "XDG_CONFIG_HOME"),
	TOOL_DIR("xdg-data", PATH_HOME, ".local/share", NULL),
	TOOL_DIR("xdg-data", PATH_HOME, ".local/share", "XDG_DATA_HOME"),
	TOOL_DIR("xdg-state", PATH_HOME, ".local/state", NULL),
	TOOL_DIR("xdg-state", PATH_HOME, ".local/state", "XDG_STATE_HOME"),
	{"xdg-runtime", PATH_ABSOLUTE, "", "XDG_RUNTIME_DIR", PATH_EXISTING_DIRECTORY},
	{"terminal", PATH_ABSOLUTE, "/dev/tty", NULL, PATH_DEVICE},
#ifdef __APPLE__
	TOOL_DIR("macos-cache", PATH_HOME, "Library/Caches", NULL),
#endif
	TOOL_DIR("npm", PATH_HOME, ".npm", "npm_config_cache"),
	TOOL_DIR("bun", PATH_HOME, ".bun/install/cache", "BUN_INSTALL_CACHE_DIR"),
	/* Skills and AI tools manage prompts, sessions, models and credentials. */
	HOME_DIR("morph-skills", ".morph/skills", NULL),
	HOME_DIR("agent-skills", ".agents/skills", NULL),
	HOME_DIR("codex", ".codex", "CODEX_HOME"),
	HOME_DIR("claude", ".claude", "CLAUDE_CONFIG_DIR"),
	HOME_DIR("gemini", ".gemini", NULL),
	HOME_DIR("continue", ".continue", NULL),
	HOME_DIR("cursor", ".cursor", NULL),
	HOME_DIR("aider", ".aider", NULL),
	HOME_DIR("opencode", ".opencode", NULL),
	HOME_DIR("ollama", ".ollama", NULL),
	ENV_DIR("ollama-models", "OLLAMA_MODELS"),
	HOME_DIR("lmstudio", ".lmstudio", NULL),
	TOOL_DIR("lmstudio-legacy", PATH_HOME, ".cache/lm-studio", NULL),
	TOOL_DIR("huggingface", PATH_HOME, ".cache/huggingface", "HF_HOME"),
	ENV_DIR("huggingface-hub", "HUGGINGFACE_HUB_CACHE"),
	ENV_DIR("huggingface-assets", "HF_ASSETS_CACHE"),
	ENV_DIR("huggingface-datasets", "HF_DATASETS_CACHE"),
	ENV_DIR("huggingface-xet", "HF_XET_CACHE"),
	ENV_DIR("transformers", "TRANSFORMERS_CACHE"),
	ENV_DIR("sentence-transformers", "SENTENCE_TRANSFORMERS_HOME"),
	TOOL_DIR("modelscope", PATH_HOME, ".cache/modelscope", "MODELSCOPE_CACHE"),
	TOOL_DIR("vllm", PATH_HOME, ".cache/vllm", "VLLM_CACHE_ROOT"),
	ENV_DIR("vllm-config", "VLLM_CONFIG_ROOT"),
	HOME_DIR("triton", ".triton", NULL),
	ENV_DIR("triton-cache", "TRITON_CACHE_DIR"),
	ENV_DIR("torch-extensions", "TORCH_EXTENSIONS_DIR"),
	ENV_DIR("torchinductor", "TORCHINDUCTOR_CACHE_DIR"),
	ENV_DIR("tiktoken", "TIKTOKEN_CACHE_DIR"),
#ifdef __APPLE__
	HOME_DIR("lmstudio", "Library/Application Support/LM Studio", NULL),
	HOME_DIR("gpt4all", "Library/Application Support/nomic.ai/GPT4All", NULL),
#endif
	/* User toolchains include installers, executable shims, config and state. */
	HOME_DIR("local-bin", ".local/bin", NULL),
	HOME_DIR("local-lib", ".local/lib", NULL),
	HOME_DIR("local-include", ".local/include", NULL),
	HOME_DIR("local-etc", ".local/etc", NULL),
	HOME_DIR("cargo", ".cargo", "CARGO_HOME"),
	HOME_DIR("rustup", ".rustup", "RUSTUP_HOME"),
	HOME_DIR("gradle", ".gradle", "GRADLE_USER_HOME"),
	HOME_DIR("maven", ".m2", "MAVEN_USER_HOME"),
	HOME_DIR("ivy", ".ivy2", NULL),
	HOME_DIR("sbt", ".sbt", NULL),
	HOME_DIR("sdkman", ".sdkman", "SDKMAN_DIR"),
	TOOL_DIR("go", PATH_LIST, "go", "GOPATH"),
	HOME_DIR("go-state", ".go", NULL),
	ENV_DIR("go-bin", "GOBIN"),
	ENV_DIR("go-modules", "GOMODCACHE"),
	HOME_DIR("nuget", ".nuget", NULL),
	ENV_DIR("nuget-packages", "NUGET_PACKAGES"),
	HOME_DIR("dotnet", ".dotnet", NULL),
	ENV_DIR("dotnet-cli", "DOTNET_CLI_HOME"),
	ENV_DIR("dotnet-sdk", "DOTNET_ROOT"),
	HOME_DIR("bun", ".bun", "BUN_INSTALL"),
	HOME_DIR("deno-install", ".deno", "DENO_INSTALL_ROOT"),
	HOME_DIR("nvm", ".nvm", "NVM_DIR"),
	HOME_DIR("volta", ".volta", "VOLTA_HOME"),
	HOME_DIR("fnm", ".local/share/fnm", "FNM_DIR"),
	HOME_DIR("pnpm", ".local/share/pnpm", "PNPM_HOME"),
	HOME_DIR("npm-global", ".npm-global", NULL),
	HOME_DIR("npm-packages", ".npm-packages", NULL),
	HOME_DIR("yarn", ".yarn", NULL),
	HOME_DIR("pyenv", ".pyenv", "PYENV_ROOT"),
	HOME_DIR("conda", ".conda", NULL),
	HOME_DIR("conda", "miniconda3", NULL),
	HOME_DIR("conda", "anaconda3", NULL),
	HOME_DIR("conda", "opt/miniconda3", NULL),
	HOME_DIR("conda", "opt/anaconda3", NULL),
	HOME_DIR("conda", "miniforge3", NULL),
	HOME_DIR("conda", "mambaforge", NULL),
	ENV_DIR("conda-env", "CONDA_PREFIX"),
	TOOL_DIR("conda-pkgs", PATH_LIST, "", "CONDA_PKGS_DIRS"),
	TOOL_DIR("conda-envs", PATH_LIST, "", "CONDA_ENVS_PATH"),
	HOME_DIR("mamba", ".local/share/mamba", "MAMBA_ROOT_PREFIX"),
	ENV_DIR("python-user", "PYTHONUSERBASE"),
	ENV_DIR("python-venv", "VIRTUAL_ENV"),
	HOME_DIR("pipx-legacy", ".local/pipx", NULL),
	ENV_DIR("pipx", "PIPX_HOME"),
	ENV_DIR("pipx-bin", "PIPX_BIN_DIR"),
	ENV_DIR("uv-python", "UV_PYTHON_INSTALL_DIR"),
	ENV_DIR("uv-tools", "UV_TOOL_DIR"),
	ENV_DIR("uv-tool-bin", "UV_TOOL_BIN_DIR"),
	HOME_DIR("virtualenvs", ".virtualenvs", "WORKON_HOME"),
	HOME_DIR("gem", ".gem", "GEM_HOME"),
	HOME_DIR("bundler", ".bundle", "BUNDLE_USER_HOME"),
	HOME_DIR("rbenv", ".rbenv", "RBENV_ROOT"),
	HOME_DIR("rvm", ".rvm", NULL),
	HOME_DIR("composer", ".composer", "COMPOSER_HOME"),
	HOME_DIR("conan", ".conan2", "CONAN_HOME"),
	HOME_DIR("conan-legacy", ".conan", NULL),
	HOME_DIR("vcpkg", ".vcpkg", NULL),
	ENV_DIR("vcpkg", "VCPKG_ROOT"),
	HOME_DIR("xmake", ".xmake", "XMAKE_GLOBALDIR"),
	HOME_DIR("cmake", ".cmake", NULL),
	HOME_DIR("asdf", ".asdf", "ASDF_DATA_DIR"),
	HOME_DIR("mise", ".local/share/mise", "MISE_DATA_DIR"),
	TOOL_DIR("julia", PATH_LIST, ".julia", "JULIA_DEPOT_PATH"),
	TOOL_DIR("r", PATH_LIST, "R", "R_LIBS_USER"),
	HOME_DIR("stack", ".stack", "STACK_ROOT"),
	HOME_DIR("cabal", ".cabal", "CABAL_DIR"),
	HOME_DIR("ghcup", ".ghcup", NULL),
	HOME_DIR("mix", ".mix", "MIX_HOME"),
	HOME_DIR("hex", ".hex", "HEX_HOME"),
	HOME_DIR("leiningen", ".lein", "LEIN_HOME"),
	HOME_DIR("dart", ".pub-cache", "PUB_CACHE"),
	ENV_DIR("flutter-sdk", "FLUTTER_ROOT"),
	HOME_DIR("android", ".android", "ANDROID_USER_HOME"),
	ENV_DIR("android-avd", "ANDROID_AVD_HOME"),
	ENV_DIR("android-sdk", "ANDROID_HOME"),
	ENV_DIR("android-sdk", "ANDROID_SDK_ROOT"),
	HOME_DIR("android-sdk", "Android/Sdk", NULL),
	/* Network clients keep downloads in the workdir and state in these roots. */
	HOME_DIR("httpie", ".httpie", "HTTPIE_CONFIG_DIR"),
	HOME_DIR("aria2", ".aria2", NULL),
	HOME_DIR("lftp", ".lftp", NULL),
	HOME_DIR("ssh-control", ".ssh/controlmasters", NULL),
	HOME_DIR("parallel", ".parallel", NULL),
	HOME_DIR("docker-buildx", ".docker/buildx", "BUILDX_CONFIG"),
	HOME_DIR("vscode-extensions", ".vscode/extensions", NULL),
	HOME_DIR("cursor-extensions", ".cursor/extensions", NULL),
	ENV_DIR("texlive", "TEXMFVAR"),
	ENV_DIR("texmf-home", "TEXMFHOME"),
	ENV_DIR("texmf-config", "TEXMFCONFIG"),
	ENV_DIR("npm-cache", "NPM_CONFIG_CACHE"),
	ENV_DIR("npm-prefix", "npm_config_prefix"),
	ENV_DIR("npm-prefix", "NPM_CONFIG_PREFIX"),
	ENV_DIR("terraform-plugins", "TF_PLUGIN_CACHE_DIR"),
	HOME_DIR("pulumi", ".pulumi", "PULUMI_HOME"),
	HOME_DIR("ansible", ".ansible", "ANSIBLE_HOME"),
	/* Tool-specific overrides may point outside the shared roots. */
	TOOL_DIR("pip", PATH_CACHE, "pip", "PIP_CACHE_DIR"),
	TOOL_DIR("uv", PATH_CACHE, "uv", "UV_CACHE_DIR"),
	TOOL_DIR("poetry", PATH_CACHE, "pypoetry", "POETRY_CACHE_DIR"),
	TOOL_DIR("yarn", PATH_CACHE, "yarn", "YARN_CACHE_FOLDER"),
	TOOL_DIR("corepack", PATH_CACHE, "node/corepack", "COREPACK_HOME"),
	TOOL_DIR("node-gyp", PATH_CACHE, "node-gyp", NULL),
	TOOL_DIR("go", PATH_CACHE, "go-build", "GOCACHE"),
	TOOL_DIR("ccache", PATH_CACHE, "ccache", "CCACHE_DIR"),
	TOOL_DIR("sccache", PATH_CACHE, "sccache", "SCCACHE_DIR"),
	TOOL_DIR("pre-commit", PATH_CACHE, "pre-commit", "PRE_COMMIT_HOME"),
	TOOL_DIR("playwright", PATH_CACHE, "ms-playwright", "PLAYWRIGHT_BROWSERS_PATH"),
	TOOL_DIR("puppeteer", PATH_CACHE, "puppeteer", "PUPPETEER_CACHE_DIR"),
	TOOL_DIR("cypress", PATH_CACHE, "Cypress", "CYPRESS_CACHE_FOLDER"),
	TOOL_DIR("electron", PATH_CACHE, "electron", "ELECTRON_CACHE"),
	TOOL_DIR("deno", PATH_CACHE, "deno", "DENO_DIR"),
	TOOL_DIR("dart", PATH_HOME, ".pub-cache", "PUB_CACHE"),
	TOOL_DIR("bazel", PATH_CACHE, "bazel", NULL),
	TOOL_DIR("composer", PATH_CACHE, "composer", "COMPOSER_CACHE_DIR"),
	TOOL_DIR("huggingface", PATH_CACHE, "huggingface/hub", "HF_HUB_CACHE"),
	TOOL_DIR("torch", PATH_CACHE, "torch", "TORCH_HOME"),
	TOOL_DIR("coursier", PATH_CACHE, "coursier", "COURSIER_CACHE"),
	TOOL_DIR("bazelisk", PATH_CACHE, "bazelisk", "BAZELISK_HOME"),
	TOOL_DIR("ruff", PATH_CACHE, "ruff", "RUFF_CACHE_DIR"),
	TOOL_DIR("mypy", PATH_CACHE, "mypy", "MYPY_CACHE_DIR"),
	TOOL_DIR("fontconfig", PATH_CACHE, "fontconfig", NULL),
	TOOL_DIR("mesa", PATH_CACHE, "mesa_shader_cache", NULL),
	TOOL_DIR("typescript", PATH_CACHE, "typescript", NULL),
	TOOL_DIR("clangd", PATH_CACHE, "clangd", NULL),
	TOOL_DIR("gopls", PATH_CACHE, "gopls", "GOPLSCACHE"),
	TOOL_DIR("helm", PATH_CACHE, "helm", "HELM_CACHE_HOME"),
	TOOL_DIR("kubectl", PATH_HOME, ".kube/cache", NULL),
	TOOL_DIR("kubectl", PATH_HOME, ".kube/http-cache", NULL),
	TOOL_DIR("httpie", PATH_CACHE, "httpie", NULL),
#ifdef __APPLE__
	TOOL_DIR("yarn", PATH_CACHE, "Yarn", NULL),
	TOOL_DIR("homebrew", PATH_CACHE, "Homebrew", "HOMEBREW_CACHE"),
	HOME_DIR("pnpm", "Library/pnpm", NULL),
	HOME_DIR("python-user", "Library/Python", NULL),
	HOME_DIR("r", "Library/R", NULL),
	HOME_DIR("android-sdk", "Library/Android/sdk", NULL),
	HOME_DIR("swiftpm", ".swiftpm", NULL),
	HOME_DIR("coursier", "Library/Application Support/Coursier", "COURSIER_HOME"),
	HOME_DIR("fnm", "Library/Application Support/fnm", NULL),
	TOOL_DIR("swiftpm", PATH_HOME, "Library/Caches/org.swift.swiftpm", NULL),
	HOME_DIR("apple-development", "Library/Developer", NULL),
	TOOL_DIR("apple-development", PATH_ABSOLUTE, "/Library/Developer", NULL),
	HOME_DIR("apple-provisioning", "Library/MobileDevice", NULL),
	HOME_DIR("apple-device-logs", "Library/Logs/CoreSimulator", NULL),
	HOME_DIR("apple-device-logs", "Library/Logs/DiagnosticReports", NULL),
	HOME_DIR("xcode", "Library/Application Support/Xcode", NULL),
#else
	TOOL_DIR("pnpm", PATH_HOME, ".local/share/pnpm/store", NULL),
	TOOL_DIR("homebrew", PATH_CACHE, "Homebrew", "HOMEBREW_CACHE"),
#endif
};

static int append_unique(nb_vec *paths, const char *path)
{
	for (size_t i = 0; i < paths->len; i++) {
		char **existing = nb_vec_at(paths, i);
		if (strcmp(*existing, path) == 0)
			return 0;
	}
	char *owned NB_CLEANUP(nb_charp_cleanup) = strdup(path);
	if (!owned)
		return NB_ERR_NOMEM;
	char **slot = nb_vec_push(paths);
	if (!slot)
		return NB_ERR_NOMEM;
	*slot = nb_charp_steal(&owned);
	return 0;
}

static int copy_paths(nb_vec *dst, char **src, int count)
{
	for (int i = 0; i < count; i++) {
		char resolved[PATH_MAX];
		if (!src || !src[i])
			continue;
		const char *path = realpath(src[i], resolved) ? resolved : src[i];
		int rc = append_unique(dst, path);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static int resolve_tool_path(const struct tool_path *entry, const char *home, char **result)
{
	const char *override = entry->env ? getenv(entry->env) : NULL;
	const char *cache = getenv("XDG_CACHE_HOME");
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc;

	rc = (nb_buf_init(&path), nb_buf_reserve(&path, 128) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	if (override && *override) {
		if (override[0] != '/')
			return 0;
		(void)nb_buf_append_cstr(&path, override);
	} else if (entry->base == PATH_ABSOLUTE) {
		if (!*entry->suffix)
			return 0;
		(void)nb_buf_append_cstr(&path, entry->suffix);
	} else if (entry->base == PATH_CACHE && cache && *cache) {
		if (cache[0] != '/')
			return 0;
		(void)nb_buf_printf(&path, "%s/%s", cache, entry->suffix);
	} else {
#ifdef __APPLE__
		const char *cache_suffix = "Library/Caches";
#else
		const char *cache_suffix = ".cache";
#endif
		if (entry->base == PATH_CACHE)
			(void)nb_buf_printf(&path, "%s/%s/%s", home, cache_suffix, entry->suffix);
		else
			(void)nb_buf_printf(&path, "%s/%s", home, entry->suffix);
	}
	if (nb_buf_status(&path))
		return nb_buf_status(&path);
	*result = (nb_buf_ok(&path) ? strdup(nb_buf_cstr(&path)) : NULL);
	return *result ? 0 : NB_ERR_NOMEM;
}

static int prepare_path(const char *path, enum path_kind kind)
{
	struct stat st;
	int rc;

	if (kind == PATH_DIRECTORY) {
		rc = nb_file_mkdir(path, true, NULL);
		if (rc != 0)
			return rc;
	}
	if (stat(path, &st) != 0)
		return nb_status_from_errno(errno, NULL);
	if (kind == PATH_DEVICE ? !S_ISCHR(st.st_mode) : !S_ISDIR(st.st_mode))
		return NB_ERR_INVAL;
	return 0;
}

static int append_tool_path(struct nb_log *logger, const struct tool_path *entry, const char *path,
			    const char *home, nb_vec *reads, nb_vec *writes, nb_vec *deletes)
{
	char resolved[PATH_MAX];
	int rc;

	if (!path || path[0] != '/' || strlen(path) >= PATH_MAX || strstr(path, "/..") ||
	    nb_path_is_within(home, path, true))
		return 0;
	if (prepare_path(path, entry->kind) != 0 || !realpath(path, resolved)) {
		nb_log_write(logger, NB_LOG_DEBUG, "sandbox: skipping unavailable %s path",
			     entry->tool);
		return 0;
	}
	if (nb_path_is_within(home, resolved, true))
		return 0;
	nb_log_write(logger, NB_LOG_DEBUG, "sandbox: tool path %s: %s", entry->tool, resolved);
	rc = append_unique(reads, resolved);
	if (rc == 0)
		rc = append_unique(writes, resolved);
	if (rc == 0 && entry->kind != PATH_DEVICE)
		rc = append_unique(deletes, resolved);
	return rc;
}

int sandbox_prepare_tool_paths(struct sandbox_config *cfg)
{
	struct sandbox_path_set paths NB_CLEANUP(sandbox_path_set_cleanup) = {0};
	const char *home_env = getenv("HOME");
	char home[PATH_MAX];
	size_t extra = sizeof(tool_paths) / sizeof(tool_paths[0]);
	int rc;

	if (!cfg || !cfg->path_policy_enabled || cfg->read_paths_count < 0 ||
	    cfg->write_paths_count < 0 || cfg->delete_paths_count < 0)
		return NB_ERR_INVAL;
	rc = sandbox_prepare_temporary_paths(cfg, NULL);
	if (rc != NB_OK)
		return rc;
	if (!home_env || !realpath(home_env, home) || strcmp(home, "/") == 0)
		return 0;
	for (size_t i = 0; i < sizeof(tool_paths) / sizeof(tool_paths[0]); i++) {
		const struct tool_path *entry = &tool_paths[i];
		const char *value = entry->env ? getenv(entry->env) : NULL;

		if (entry->base == PATH_LIST && value) {
			for (; *value; value++) {
				if (*value == ':')
					extra++;
			}
		}
	}
	rc = (nb_vec_init(&paths.read, sizeof(*cfg->read_paths)),
	      nb_vec_reserve(&paths.read, (size_t)cfg->read_paths_count + extra) ? 0
										 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_vec_init(&paths.write, sizeof(*cfg->write_paths)),
		      nb_vec_reserve(&paths.write, (size_t)cfg->write_paths_count + extra)
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_vec_init(&paths.remove, sizeof(*cfg->delete_paths)),
		      nb_vec_reserve(&paths.remove, (size_t)cfg->delete_paths_count + extra)
			      ? 0
			      : NB_ERR_NOMEM);
	if (rc == 0)
		rc = copy_paths(&paths.read, cfg->read_paths, cfg->read_paths_count);
	if (rc == 0)
		rc = copy_paths(&paths.write, cfg->write_paths, cfg->write_paths_count);
	if (rc == 0)
		rc = copy_paths(&paths.remove, cfg->delete_paths, cfg->delete_paths_count);
	if (rc != 0)
		return rc;
	for (size_t i = 0; i < sizeof(tool_paths) / sizeof(tool_paths[0]); i++) {
		const struct tool_path *entry = &tool_paths[i];
		char *path NB_CLEANUP(nb_charp_cleanup) = NULL;
		const char *value = entry->env ? getenv(entry->env) : NULL;

		if (entry->base == PATH_LIST && value && *value) {
			char *parts NB_CLEANUP(nb_charp_cleanup) = strdup(value);
			char *part;
			char *save = NULL;

			if (!parts)
				return NB_ERR_NOMEM;
			for (part = strtok_r(parts, ":", &save); part;
			     part = strtok_r(NULL, ":", &save)) {
				rc = append_tool_path(cfg ? cfg->logger : NULL, entry, part, home,
						      &paths.read, &paths.write, &paths.remove);
				if (rc != 0)
					return rc;
			}
			continue;
		}
		rc = resolve_tool_path(entry, home, &path);
		if (rc == 0)
			rc = append_tool_path(cfg ? cfg->logger : NULL, entry, path, home,
					      &paths.read, &paths.write, &paths.remove);
		if (rc != 0)
			return rc;
	}
	if (paths.read.len > INT_MAX || paths.write.len > INT_MAX || paths.remove.len > INT_MAX)
		return NB_ERR_RANGE;
	sandbox_config_paths_move(cfg, &paths);
	return 0;
}
