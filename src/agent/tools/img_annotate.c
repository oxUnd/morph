#include "img_annotate.h"
#include "util/log.h"
#include "util/file.h"
#include "util/error.h"
#include "cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

#define MAX_IMAGES 32

static img_annotate_pause_fn g_pause_fn;
static img_annotate_resume_fn g_resume_fn;
static void *g_cb_user_data;

/*
 * Probe for the morph-editor binary. Search order:
 *   1. MORPH_EDITOR env var (absolute path)
 *   2. <cwd>/../morph-editor/build/morph-editor (dev layout)
 *   3. $PATH via execlp fallback (indicated by returning "")
 *
 * Returns a heap-allocated path the caller must free, or NULL on
 * unrecoverable error.
 */
static char *find_editor_binary(void)
{
	const char *env;
	char buf[4096];
	char cwd[4096];

	env = getenv("MORPH_EDITOR");
	if (env && env[0]) {
		if (file_exists(env))
			return strdup(env);
		log_warn("MORPH_EDITOR=%s not found, falling back", env);
	}

	/*
	 * Development layout: morph and morph-editor sit side by
	 * side under the same parent dir.  When the user runs
	 * morph from its project root the CWD is .../morph, so
	 * ../morph-editor/build/morph-editor points to the
	 * sibling build.
	 */
	if (getcwd(cwd, sizeof(cwd))) {
		snprintf(buf, sizeof(buf),
			 "%s/../morph-editor/build/morph-editor", cwd);
		if (file_exists(buf))
			return strdup(buf);
	}

	log_info("img_annotate: morph-editor not found locally, "
		 "will search $PATH");
	return strdup("");
}

static int img_annotate_exec(const char *args_json, char **result_json,
			     void *user_data)
{
	char *paths[MAX_IMAGES] = {NULL};
	int npaths = 0;
	char *editor_path = NULL;
	pid_t pid;
	int pipefd[2];
	int status;
	int rc = 0;
	int i;

	(void)user_data;
	if (!result_json)
		return -EINVAL;

	if (!args_json || !*args_json) {
		*result_json = strdup(
			"{\"error\":\"missing arguments. "
			"Usage: img_annotate({\\\"path\\\": \\\"image.jpg\\\"}) "
			"or img_annotate({\\\"paths\\\": "
			"[\\\"a.jpg\\\",\\\"b.jpg\\\"]})\"}");
		MORPH_RETURN(-EINVAL);
	}

	{
		cJSON *root = cJSON_Parse(args_json);

		if (!root) {
			*result_json = strdup(
				"{\"error\":\"invalid JSON arguments\"}");
			MORPH_RETURN(-EINVAL);
		}

		/*
		 * Accept either "paths" (array of strings) or
		 * "path" (single string, backward-compatible).
		 */
		cJSON *arr = cJSON_GetObjectItem(root, "paths");
		if (cJSON_IsArray(arr)) {
			int sz = cJSON_GetArraySize(arr);
			if (sz <= 0) {
				cJSON_Delete(root);
				*result_json = strdup(
					"{\"error\":\"'paths' array is empty\"}");
				MORPH_RETURN(-EINVAL);
			}
			if (sz > MAX_IMAGES) {
				cJSON_Delete(root);
				char err[128];
				snprintf(err, sizeof(err),
					 "{\"error\":\"too many images "
					 "(max %d)\"}", MAX_IMAGES);
				*result_json = strdup(err);
				MORPH_RETURN(-EINVAL);
			}
			for (i = 0; i < sz; i++) {
				cJSON *item = cJSON_GetArrayItem(arr, i);
				if (!cJSON_IsString(item) ||
				    !item->valuestring ||
				    !item->valuestring[0]) {
					cJSON_Delete(root);
					for (int j = 0; j < npaths; j++)
						free(paths[j]);
					*result_json = strdup(
						"{\"error\":\"'paths' contains "
						"invalid entry\"}");
					MORPH_RETURN(-EINVAL);
				}
				paths[npaths++] = strdup(item->valuestring);
			}
		} else {
			cJSON *p = cJSON_GetObjectItem(root, "path");
			if (!cJSON_IsString(p) || !p->valuestring ||
			    !p->valuestring[0]) {
				cJSON_Delete(root);
				*result_json = strdup(
					"{\"error\":\"missing 'path' or "
					"'paths' parameter. "
					"Usage: img_annotate({\\\"path\\\": "
					"\\\"image.jpg\\\"}) or "
					"img_annotate({\\\"paths\\\": "
					"[\\\"a.jpg\\\",\\\"b.jpg\\\"]})\"}");
				MORPH_RETURN(-EINVAL);
			}
			paths[npaths++] = strdup(p->valuestring);
		}
		cJSON_Delete(root);
	}

	for (i = 0; i < npaths; i++) {
		if (!file_exists(paths[i])) {
			char err[1100];
			snprintf(err, sizeof(err),
				 "{\"error\":\"image file not found: %s\"}",
				 paths[i]);
			*result_json = strdup(err);
			for (int j = 0; j < npaths; j++)
				free(paths[j]);
			MORPH_RETURN(-ENOENT);
		}
	}

	editor_path = find_editor_binary();
	if (!editor_path) {
		for (i = 0; i < npaths; i++)
			free(paths[i]);
		*result_json = strdup(
			"{\"error\":\"could not locate morph-editor binary. "
			"Set MORPH_EDITOR env var or install morph-editor.\"}");
		MORPH_RETURN(-ENOENT);
	}

	if (pipe(pipefd) < 0) {
		free(editor_path);
		for (i = 0; i < npaths; i++)
			free(paths[i]);
		*result_json = strdup(
			"{\"error\":\"pipe() failed\"}");
		MORPH_RETURN(-errno);
	}

	if (g_pause_fn)
		g_pause_fn(g_cb_user_data);

	fflush(stdout);
	fflush(stderr);

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		if (g_resume_fn)
			g_resume_fn(g_cb_user_data);
		free(editor_path);
		for (i = 0; i < npaths; i++)
			free(paths[i]);
		*result_json = strdup(
			"{\"error\":\"fork() failed\"}");
		MORPH_RETURN(-errno);
	}

	if (pid == 0) {
		/*
		 * Child process: exec morph-editor with all
		 * image paths.  morph-editor "open" subcommand
		 * accepts multiple paths for multi-image
		 * annotation.
		 *
		 * Redirect stdout to the pipe so we can capture
		 * the annotation JSON that morph-editor prints on
		 * exit (editor_dump_on_quit).  stderr and stdin
		 * are inherited directly so termbox2 can use
		 * /dev/tty for rendering and input.
		 */
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(126);
		close(pipefd[1]);

		/*
		 * Build argv: morph-editor open path1 path2 ...
		 * +1 for program name, +1 for "open", +npaths,
		 * +1 for NULL sentinel.
		 */
		{
			int argc = 2 + npaths + 1;
			char **argv = malloc(sizeof(char *) *
					     (size_t)argc);
			if (!argv)
				_exit(126);
			argv[0] = "morph-editor";
			argv[1] = "open";
			for (i = 0; i < npaths; i++)
				argv[2 + i] = paths[i];
			argv[2 + npaths] = NULL;

			if (editor_path[0])
				execv(editor_path, argv);
			else
				execvp("morph-editor", argv);
			_exit(127);
		}
	}

	/* Parent: close write end, read annotation JSON from child */
	close(pipefd[1]);

	{
		char *buf = NULL;
		size_t buf_len = 0;
		size_t buf_cap = 0;
		char tmp[4096];
		ssize_t n;

		while ((n = read(pipefd[0], tmp, sizeof(tmp))) > 0) {
			if (buf_len + (size_t)n + 1 > buf_cap) {
				size_t new_cap = buf_cap ? buf_cap * 2 : 4096;
				char *new_buf;

				while (new_cap < buf_len + (size_t)n + 1)
					new_cap *= 2;
				new_buf = realloc(buf, new_cap);
				if (!new_buf) {
					free(buf);
					close(pipefd[0]);
					waitpid(pid, &status, 0);
					if (g_resume_fn)
						g_resume_fn(g_cb_user_data);
					free(editor_path);
					for (i = 0; i < npaths; i++)
						free(paths[i]);
					*result_json = strdup(
						"{\"error\":\"out of memory "
						"reading editor output\"}");
					MORPH_RETURN(-ENOMEM);
				}
				buf = new_buf;
				buf_cap = new_cap;
			}
			memcpy(buf + buf_len, tmp, (size_t)n);
			buf_len += (size_t)n;
		}
		close(pipefd[0]);

		if (buf) {
			buf[buf_len] = '\0';
			/*
			 * Strip trailing newlines — morph-editor
			 * appends \n via fprintf + editor_dump_on_quit.
			 */
			while (buf_len > 0 && buf[buf_len - 1] == '\n')
				buf[--buf_len] = '\0';
		}

		/*
		 * Reap the child process.
		 */
		while (waitpid(pid, &status, 0) < 0) {
			if (errno != EINTR)
				break;
		}

		if (g_resume_fn)
			g_resume_fn(g_cb_user_data);

		if (!buf || buf_len == 0) {
			/*
			 * Editor exited without producing output
			 * (e.g. user quit before annotating, or
			 * image failed to load).
			 */
			free(buf);
			free(editor_path);
			for (i = 0; i < npaths; i++)
				free(paths[i]);
			if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
				char err[256];
				snprintf(err, sizeof(err),
					 "{\"error\":\"morph-editor exited "
					 "with code %d\"}",
					 WEXITSTATUS(status));
				*result_json = strdup(err);
				MORPH_RETURN(-EIO);
			}
			*result_json = strdup(
				"{\"images\":[],\"bboxes\":[],\"arrows\":[]}");
			return 0;
		}

		/*
		 * Validate that the output is valid JSON containing
		 * annotation data.  morph-editor outputs:
		 *   {"images":[...],"bboxes":[...],"arrows":[...]}
		 * If valid, pass it through as-is.  Otherwise wrap
		 * it in an error envelope.
		 */
		{
			cJSON *root = cJSON_Parse(buf);

			if (root) {
				cJSON_Delete(root);
				*result_json = buf;
				free(editor_path);
				for (i = 0; i < npaths; i++)
					free(paths[i]);
				return 0;
			}
		}

		/*
		 * Not valid JSON — wrap in error object so the LLM
		 * can see what happened.
		 */
		{
			cJSON *out = cJSON_CreateObject();
			char *tmp_str;

			cJSON_AddStringToObject(out, "error",
						"editor output was not "
						"valid JSON");
			cJSON_AddStringToObject(out, "raw_output", buf);
			tmp_str = cJSON_PrintUnformatted(out);
			cJSON_Delete(out);
			free(buf);
			free(editor_path);
			for (i = 0; i < npaths; i++)
				free(paths[i]);
			*result_json = tmp_str;
			MORPH_RETURN(-MORPH_ERR_PARSE);
		}
	}

	return rc;
}

int img_annotate_init(struct tool_registry *reg,
		      img_annotate_pause_fn pause_fn,
		      img_annotate_resume_fn resume_fn,
		      void *user_data)
{
	if (!reg)
		return -EINVAL;

	g_pause_fn = pause_fn;
	g_resume_fn = resume_fn;
	g_cb_user_data = user_data;

	return tool_register(reg, "img_annotate",
		"Open one or more images in the interactive terminal "
		"image editor (morph-editor) for manual annotation. "
		"Supports two annotation types: "
		"1) bounding boxes (bboxes) to label regions of "
		"interest; "
		"2) arrows to specify compositing — the object at "
		"the arrow's source end will be blended/composited "
		"into the target position the arrow points to "
		"(cross-image compositing is supported). "
		"The editor takes over the terminal: the user draws "
		"bboxes and arrows with the mouse, labels them, then "
		"presses 'q' to quit. On exit the annotations are "
		"returned as JSON: "
		"{\"images\":[{\"path\":\"...\",\"width\":N,"
		"\"height\":N},...],"
		"\"bboxes\":[{\"id\":1,\"image_index\":N,"
		"\"x\":...,\"y\":...,\"w\":...,\"h\":...,"
		"\"label\":\"...\"},...],"
		"\"arrows\":[{\"id\":1,\"from\":{\"image_index\":N,"
		"\"x\":...,\"y\":...},\"to\":{\"image_index\":N,"
		"\"x\":...,\"y\":...},\"label\":\"...\","
		"\"color\":\"#rrggbb\"},...]}. "
		"Use this tool when you need the user to manually "
		"identify regions of interest or specify "
		"compositing instructions across images "
		"(e.g. 'mark objects in these photos' or "
		"'composite the chair from image 1 into image 2'). "
		"Do NOT use this for automated image operations "
		"like resize, crop, or format conversion.",
		"{\"type\":\"object\",\"properties\":{"
		"\"path\":{\"type\":\"string\","
		"\"description\":\"Path to a single image file to "
		"annotate (use either 'path' or 'paths', not both)\"},"
		"\"paths\":{\"type\":\"array\",\"items\":{"
		"\"type\":\"string\"},"
		"\"description\":\"Array of image file paths to "
		"annotate together (use either 'path' or 'paths', "
		"not both)\"}"
		"},\"required\":[]}",
		img_annotate_exec, NULL, NULL);
}
