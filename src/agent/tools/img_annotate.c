#include "util/signals.h"
#include "agent/tool_runtime.h"
#include "img_annotate.h"
#include "agent/tool_context.h"
#include "util/log.h"
#include "util/file.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/json.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

#define MAX_IMAGES 32

struct img_annotate_context {
	img_annotate_pause_fn pause_fn;
	img_annotate_resume_fn resume_fn;
	void *cb_user_data;
	struct tool_context *tctx;
};

static void img_annotate_context_destroy(void *user_data)
{
	free(user_data);
}

/*
 * Probe for the morph-editor binary. Search order:
 *   1. MORPH_EDITOR env var (absolute path)
 *   2. <cwd>/../morph-editor/build/morph-editor (dev layout)
 *   3. $PATH via execlp fallback (indicated by returning "")
 *
 * Returns a heap-allocated path the caller must free, or NULL on
 * unrecoverable error.
 */
static char *find_editor_binary(struct morph_log *logger)
{
	const char *env;
	char buf[PATH_MAX + 64];
	char cwd[PATH_MAX];

	env = getenv("MORPH_EDITOR");
	if (env && env[0]) {
		if (file_exists(env))
			return strdup(env);
		log_warn(logger, "MORPH_EDITOR=%s not found, falling back", env);
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

	log_info(logger, "img_annotate: morph-editor not found locally, "
			 "will search $PATH");
	return strdup("");
}

static int img_annotate_exec(const char *args_json, struct tool_result *result,
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
	struct img_annotate_context *ctx = user_data;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;

	if (!result)
		return -EINVAL;

	if (!args_json || !*args_json) {
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"missing arguments. "
			"Usage: img_annotate({\\\"path\\\": \\\"image.jpg\\\"}) "
			"or img_annotate({\\\"paths\\\": "
			"[\\\"a.jpg\\\",\\\"b.jpg\\\"]})\"}"));
		return -EINVAL;
	}

	{
		struct json *root = json_parse_prefix(args_json, NULL, NULL);

		if (!root) {
			(void)tool_result_success_json_text(result, strdup(
				"{\"error\":\"invalid JSON arguments\"}"));
			return -EINVAL;
		}

		/*
		 * Accept either "paths" (array of strings) or
		 * "path" (single string, backward-compatible).
		 */
		struct json *arr = json_member_ci(root, "paths");
		if (json_is_array_value(arr)) {
			int sz = json_count(arr);
			if (sz <= 0) {
				json_free(root);
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"'paths' array is empty\"}"));
				return -EINVAL;
			}
			if (sz > MAX_IMAGES) {
				json_free(root);
				char err[128];
				snprintf(err, sizeof(err),
					 "{\"error\":\"too many images "
					 "(max %d)\"}", MAX_IMAGES);
				(void)tool_result_success_json_text(result, strdup(err));
				return -EINVAL;
			}
			for (i = 0; i < sz; i++) {
				struct json *item = json_at(arr, i);
				if (!json_is_string_value(item) || !json_string_value(item) ||
				    !json_string_value(item)[0]) {
					json_free(root);
					for (int j = 0; j < npaths; j++)
						free(paths[j]);
					(void)tool_result_success_json_text(result, strdup(
						"{\"error\":\"'paths' contains "
						"invalid entry\"}"));
					return -EINVAL;
				}
				paths[npaths++] = strdup(json_string_value(item));
			}
		} else {
			struct json *p = json_member_ci(root, "path");
			if (!json_is_string_value(p) || !json_string_value(p) ||
			    !json_string_value(p)[0]) {
				json_free(root);
				(void)tool_result_success_json_text(result, strdup(
					"{\"error\":\"missing 'path' or "
					"'paths' parameter. "
					"Usage: img_annotate({\\\"path\\\": "
					"\\\"image.jpg\\\"}) or "
					"img_annotate({\\\"paths\\\": "
					"[\\\"a.jpg\\\",\\\"b.jpg\\\"]})\"}"));
				return -EINVAL;
			}
			paths[npaths++] = strdup(json_string_value(p));
		}
		json_free(root);
	}

	for (i = 0; i < npaths; i++) {
		char resolved[PATH_MAX];
		if (tctx) {
			rc = tool_context_authorize_path(
				tctx, TOOL_PATH_READ, paths[i],
				resolved, sizeof(resolved));
			if (rc < 0) {
				char err[1100];
				if (rc == -ENOENT)
					snprintf(err, sizeof(err),
						 "{\"error\":\"image file not found: %s\"}",
						 paths[i]);
				else
					snprintf(err, sizeof(err),
						 "{\"error\":\"read path outside workspace: permission denied\"}");
				(void)tool_result_success_json_text(result, strdup(err));
				for (int j = 0; j < npaths; j++)
					free(paths[j]);
				return rc;
			}
			free(paths[i]);
			paths[i] = strdup(resolved);
			if (!paths[i]) {
				for (int j = 0; j < npaths; j++)
					free(paths[j]);
				return -ENOMEM;
			}
		} else if (!file_exists(paths[i])) {
			char err[1100];
			snprintf(err, sizeof(err),
				 "{\"error\":\"image file not found: %s\"}",
				 paths[i]);
			(void)tool_result_success_json_text(result, strdup(err));
			for (int j = 0; j < npaths; j++)
				free(paths[j]);
			return -ENOENT;
		}
	}

	editor_path =
		find_editor_binary(result && result->runtime ? result->runtime->http.logger : NULL);
	if (!editor_path) {
		for (i = 0; i < npaths; i++)
			free(paths[i]);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"could not locate morph-editor binary. "
			"Set MORPH_EDITOR env var or install morph-editor.\"}"));
		return -ENOENT;
	}

	if (pipe(pipefd) < 0) {
		int err = errno;
		free(editor_path);
		for (i = 0; i < npaths; i++)
			free(paths[i]);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"pipe() failed\"}"));
		return -err;
	}

	if (ctx && ctx->pause_fn)
		ctx->pause_fn(ctx->cb_user_data);

	fflush(stdout);
	fflush(stderr);

	pid = fork();
	if (pid < 0) {
		int err = errno;
		close(pipefd[0]);
		close(pipefd[1]);
		if (ctx && ctx->resume_fn)
			ctx->resume_fn(ctx->cb_user_data);
		free(editor_path);
		for (i = 0; i < npaths; i++)
			free(paths[i]);
		(void)tool_result_success_json_text(result, strdup(
			"{\"error\":\"fork() failed\"}"));
		return -err;
	}

	if (pid == 0) {
		morph_signals_reset_child();
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
		morph_buf_t buf;
		int buf_init_rc = morph_buf_init(&buf, 4096);

		if (buf_init_rc != 0) {
			close(pipefd[0]);
			waitpid(pid, &status, 0);
			if (ctx && ctx->resume_fn)
				ctx->resume_fn(ctx->cb_user_data);
			free(editor_path);
			for (i = 0; i < npaths; i++)
				free(paths[i]);
			(void)tool_result_success_json_text(result, strdup(
				"{\"error\":\"out of memory "
				"reading editor output\"}"));
			return -ENOMEM;
		}

		char tmp[4096];
		ssize_t n;

		while ((n = read(pipefd[0], tmp, sizeof(tmp))) > 0) {
			if (buf.len + (size_t)n + 1 > buf.cap) {
				int grow_rc = morph_buf_reserve(&buf, (size_t)n + 1);
				if (grow_rc != 0) {
					morph_buf_cleanup(&buf);
					close(pipefd[0]);
					waitpid(pid, &status, 0);
					if (ctx && ctx->resume_fn)
						ctx->resume_fn(ctx->cb_user_data);
					free(editor_path);
					for (i = 0; i < npaths; i++)
						free(paths[i]);
					(void)tool_result_success_json_text(result, strdup(
						"{\"error\":\"out of memory "
						"reading editor output\"}"));
					return -ENOMEM;
				}
			}
			memcpy(buf.data + buf.len, tmp, (size_t)n);
			buf.len += (size_t)n;
		}
		close(pipefd[0]);
		buf.data[buf.len] = '\0';

		/*
		 * Strip trailing newlines — morph-editor
		 * appends \n via fprintf + editor_dump_on_quit.
		 */
		while (buf.len > 0 && buf.data[buf.len - 1] == '\n')
			buf.data[--buf.len] = '\0';

		/*
		 * Reap the child process.
		 */
		while (waitpid(pid, &status, 0) < 0) {
			if (errno != EINTR)
				break;
		}

		if (ctx && ctx->resume_fn)
			ctx->resume_fn(ctx->cb_user_data);

		if (buf.len == 0) {
			/*
			 * Editor exited without producing output
			 * (e.g. user quit before annotating, or
			 * image failed to load).
			 */
			morph_buf_cleanup(&buf);
			free(editor_path);
			for (i = 0; i < npaths; i++)
				free(paths[i]);
			if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
				char err[256];
				snprintf(err, sizeof(err),
					 "{\"error\":\"morph-editor exited "
					 "with code %d\"}",
					 WEXITSTATUS(status));
				(void)tool_result_success_json_text(result, strdup(err));
				return -EIO;
			}
			(void)tool_result_success_json_text(result, strdup(
				"{\"images\":[],\"bboxes\":[],\"arrows\":[]}"));
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
			struct json *root = json_parse_prefix(buf.data, NULL, NULL);

			if (root) {
				/*
				 * Build compositing_plan from arrows:
				 * count how many arrows point to each
				 * image (to.image_index), find the
				 * main image (most to's, tie-break by
				 * smallest image_index), then emit a
				 * structured plan the LLM can use for
				 * image generation.
				 */
				struct json *arrows = json_member_ci(root, "arrows");
				if (json_is_array_value(arrows) && json_count(arrows) > 0) {
					int to_count[MAX_IMAGES] = {0};
					int nimages = 0;
					int main_idx = -1;
					int max_count = 0;
					int ai;

					struct json *imgs = json_member_ci(root, "images");
					if (json_is_array_value(imgs))
						nimages = json_count(imgs);

					for (ai = 0; ai < json_count(arrows); ai++) {
						struct json *a = json_at(arrows, ai);
						struct json *to = json_member_ci(a, "to");
						if (!to) continue;
						struct json *ti = json_member_ci(to, "image_index");
						if (json_is_number_value(ti) &&
						    json_int_value(ti) >= 0 &&
						    json_int_value(ti) < nimages &&
						    json_int_value(ti) < MAX_IMAGES)
							to_count[json_int_value(ti)]++;
					}

					for (ai = 0;
					     ai < nimages && ai < MAX_IMAGES;
					     ai++) {
						if (to_count[ai] > max_count) {
							max_count = to_count[ai];
							main_idx = ai;
						}
					}

					if (main_idx >= 0 && max_count > 0) {
						struct json *plan = json_object();
						struct json *sources = json_array();
						struct json *mbboxes = json_array();

						json_add_number(plan, "main_image_index", main_idx);

						struct json *mi = json_at(imgs, main_idx);
						if (mi) {
							struct json *mp =
								json_member_ci(mi, "path");
							if (mp && json_is_string_value(mp))
								json_add_string(
									plan, "main_image_path",
									json_string_value(mp));
						}

						for (ai = 0; ai < json_count(arrows); ai++) {
							struct json *a = json_at(arrows, ai);
							struct json *to = json_member_ci(a, "to");
							struct json *from =
								json_member_ci(a, "from");
							if (!to || !from)
								continue;
							struct json *ti =
								json_member_ci(to, "image_index");
							if (!json_is_number_value(ti))
								continue;
							if (json_int_value(ti) != main_idx)
								continue;

							struct json *src = json_object();
							struct json *fi =
								json_member_ci(from, "image_index");
							if (fi && json_is_number_value(fi)) {
								json_add_number(src,
										"from_image_index",
										json_int_value(fi));
								struct json *si = json_at(
									imgs, json_int_value(fi));
								if (si) {
									struct json *sp =
										json_member_ci(
											si, "path");
									if (sp &&
									    json_is_string_value(
										    sp))
										json_add_string(
											src,
											"from_"
											"image_"
											"path",
											json_string_value(
												sp));
								}
							}

							struct json *fx = json_member_ci(from, "x");
							struct json *fy = json_member_ci(from, "y");
							if (fx && json_is_number_value(fx))
								json_add_number(
									src, "from_x",
									json_number_value(fx));
							if (fy && json_is_number_value(fy))
								json_add_number(
									src, "from_y",
									json_number_value(fy));

							struct json *tx = json_member_ci(to, "x");
							struct json *ty = json_member_ci(to, "y");
							if (tx && json_is_number_value(tx))
								json_add_number(
									src, "to_x",
									json_number_value(tx));
							if (ty && json_is_number_value(ty))
								json_add_number(
									src, "to_y",
									json_number_value(ty));

							struct json *lbl =
								json_member_ci(a, "label");
							if (lbl && json_is_string_value(lbl))
								json_add_string(
									src, "label",
									json_string_value(lbl));

							json_array_add(sources, src);
						}

						json_object_add(plan, "sources", sources);

						struct json *bboxes =
							json_member_ci(root, "bboxes");
						if (json_is_array_value(bboxes)) {
							for (ai = 0; ai < json_count(bboxes);
							     ai++) {
								struct json *b =
									json_at(bboxes, ai);
								struct json *bi = json_member_ci(
									b, "image_index");
								if (bi &&
								    json_is_number_value(bi) &&
								    json_int_value(bi) == main_idx)
									json_array_add(
										mbboxes,
										json_clone(b, 1));
							}
						}
						json_object_add(plan, "main_bboxes", mbboxes);

						json_object_add(root, "compositing_plan", plan);
					}
				}

				{
					char *out = json_stringify(root);
					json_free(root);
					morph_buf_cleanup(&buf);
					(void)tool_result_success_json_text(result, out);
				}
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
			struct json *out = json_object();
			char *tmp_str;

			json_add_string(out, "error",
					"editor output was not "
					"valid JSON");
			json_add_string(out, "raw_output", buf.data);
			tmp_str = json_stringify(out);
			json_free(out);
			morph_buf_cleanup(&buf);
			free(editor_path);
			for (i = 0; i < npaths; i++)
				free(paths[i]);
			(void)tool_result_success_json_text(result, tmp_str);
			return MORPH_ERR_PARSE;
		}
	}

	return rc;
}

int img_annotate_init(struct tool_registry *reg,
		      img_annotate_pause_fn pause_fn,
		      img_annotate_resume_fn resume_fn,
		      void *user_data,
		      struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;

	struct img_annotate_context *ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->pause_fn = pause_fn;
	ctx->resume_fn = resume_fn;
	ctx->cb_user_data = user_data;
	ctx->tctx = tctx;

	int rc = tool_register(reg, &(struct tool_spec){
		.origin = TOOL_ORIGIN_BUILTIN,
		.flags = TOOL_FLAG_MEDIA,
		.name = "img_annotate",
		.description = "Open one or more images in the interactive terminal "
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
		"When arrows are present the output also contains "
		"a 'compositing_plan' field: 'main_image_index' "
		"is the image that receives the most arrow "
		"pointers (the base/main image); 'sources' lists "
		"each source image with its from/to coordinates "
		"and label; 'main_bboxes' lists all bboxes on "
		"the main image. Use compositing_plan to drive "
		"image generation: use the main image as the "
		"base and composite source content at the "
		"indicated positions. "
		"Do NOT use this for automated image operations "
		"like resize, crop, or format conversion.", .input_schema = "{\"type\":\"object\",\"properties\":{"
		"\"path\":{\"type\":\"string\","
		"\"description\":\"Path to a single image file to "
		"annotate (use either 'path' or 'paths', not both)\"},"
		"\"paths\":{\"type\":\"array\",\"items\":{"
		"\"type\":\"string\"},"
		"\"description\":\"Array of image file paths to "
		"annotate together (use either 'path' or 'paths', "
		"not both)\"}"
		"},\"required\":[]}", .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA, .exec = img_annotate_exec, .user_data = ctx, .user_data_destroy = img_annotate_context_destroy });
	if (rc != 0)
		free(ctx);
	return rc;
}
