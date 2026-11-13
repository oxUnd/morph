#include "sapi/cli/presentation/share.h"
#include "nb/err.h"
#include "cJSON.h"
#include "media/image.h"
#include "runtime/resources.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>
#include <morph_markdown_html.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *cli_share_text(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
	return cJSON_IsString(value) ? value->valuestring : "";
}

static int cli_share_escape(nb_buf *html, const char *text)
{
	if (nb_buf_status(html))
		return nb_buf_status(html);
	char *safe = morph_terminal_sanitize_dup(text, strlen(text), MORPH_TERMINAL_TEXT_MULTILINE,
						 NULL);
	if (!safe)
		return NB_ERR_NOMEM;
	for (const char *p = safe; *p; p++) {
		switch (*p) {
		case '&':
			nb_buf_append_cstr(html, "&amp;");
			break;
		case '<':
			nb_buf_append_cstr(html, "&lt;");
			break;
		case '>':
			nb_buf_append_cstr(html, "&gt;");
			break;
		case '"':
			nb_buf_append_cstr(html, "&quot;");
			break;
		case '\'':
			nb_buf_append_cstr(html, "&#39;");
			break;
		default:
			nb_buf_append_char(html, *p);
			break;
		}
	}
	free(safe);
	return nb_buf_status(html);
}

/* Export URL policy: omit image resources; permit only ordinary external links.
 * Fragment references are preserved by the Markdown exporter itself. */
static int cli_share_resolve_resource(const struct morph_md_html_resource *resource,
				      const char **out_url, void *user_data)
{
	int *images_omitted = user_data;
	if (strcmp(resource->tag, "img") == 0 && strcmp(resource->attribute, "src") == 0) {
		*images_omitted = 1;
		*out_url = "data:,";
	} else if (strcmp(resource->attribute, "href") == 0 ||
		   strcmp(resource->attribute, "xlink:href") == 0) {
		if (strncasecmp(resource->url, "https:", 6) != 0 &&
		    strncasecmp(resource->url, "http:", 5) != 0 &&
		    strncasecmp(resource->url, "mailto:", 7) != 0)
			*out_url = "";
	} else {
		*out_url = "";
	}
	return 0;
}

static int cli_share_markdown(nb_buf *html, const char *text)
{
	char font[PATH_MAX];
	int rc = morph_data_find(font, sizeof(font), "fonts/STIXTwoMath-Regular.ttf");
	if (rc)
		return rc;
	struct morph_md_engine_options engine_options = {
		.features = MORPH_MD_FEATURE_GFM | MORPH_MD_FEATURE_MATH,
		.html_policy = MORPH_MD_HTML_STRIP,
	};
	int images_omitted = 0;
	struct morph_md_html_options html_options = {
		.math_font_path = font,
		.resolve_resource = cli_share_resolve_resource,
		.user_data = &images_omitted,
	};
	struct morph_md_engine *engine = NULL;
	struct morph_md_doc *document = NULL;
	char *fragment = NULL;
	rc = morph_md_engine_create(&engine_options, &engine);
	if (!rc)
		rc = morph_md_engine_append(engine, text, strlen(text), 1);
	if (!rc)
		rc = morph_md_engine_snapshot(engine, &document);
	if (!rc)
		rc = morph_md_doc_to_html(document, &html_options, &fragment);
	if (rc) {
		if (rc != NB_ERR_NOMEM && rc != NB_ERR_INVAL)
			rc = (NB_ERR_FORMAT);
	} else {
		rc = (nb_buf_append_cstr(html, fragment) ? 0 : NB_ERR_NOMEM);
		if (!rc && images_omitted)
			rc = (nb_buf_append_cstr(
				      html, "<p class=note>Images omitted from this export.</p>")
				      ? 0
				      : NB_ERR_NOMEM);
	}
	morph_md_free(fragment);
	morph_md_doc_release(document);
	morph_md_engine_destroy(engine);
	return rc;
}

static int cli_share_questions(const cJSON *questions, nb_buf *html)
{
	nb_buf_append_cstr(html, "<section><h2 class=label>YOU</h2>");
	const cJSON *question;
	cJSON_ArrayForEach (question, questions) {
		nb_buf_append_cstr(html, "<div class=question>");
		int rc = cli_share_escape(html,
					  cJSON_IsString(question) ? question->valuestring : "");
		if (rc)
			return rc;
		nb_buf_append_cstr(html, "</div>");
	}
	nb_buf_append_cstr(html, "</section>");
	return nb_buf_status(html);
}

static int cli_share_step_html(const cJSON *step, nb_buf *html)
{
	nb_buf_append_cstr(html, "<li><div class=step-title>");
	int rc = cli_share_escape(html, cli_share_text(step, "name"));
	if (rc)
		return rc;
	nb_buf_append_cstr(html, "<span class=status>");
	rc = cli_share_escape(html, cli_share_text(step, "status"));
	if (rc)
		return rc;
	nb_buf_append_cstr(html, "</span></div><details class=tool-output>"
				 "<summary>View result</summary><pre>");
	rc = cli_share_escape(html, cli_share_text(step, "content"));
	if (rc)
		return rc;
	nb_buf_append_cstr(html, "</pre>");
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(step, "truncated")))
		nb_buf_append_cstr(html, "<p class=note>This result was shortened "
					 "before it was stored.</p>");
	nb_buf_append_cstr(html, "</details></li>");
	return nb_buf_status(html);
}

static int cli_share_process(const cJSON *steps, nb_buf *html)
{
	int count = cJSON_GetArraySize(steps);
	if (count == 0)
		return 0;
	nb_buf_printf(html,
		      "<details class=process><summary>Execution process · "
		      "%d step%s</summary><ol class=steps>",
		      count, count == 1 ? "" : "s");
	const cJSON *step;
	cJSON_ArrayForEach (step, steps) {
		int rc = cli_share_step_html(step, html);
		if (rc)
			return rc;
	}
	nb_buf_append_cstr(html, "</ol></details>");
	return nb_buf_status(html);
}

/* Stream an asset into the document; keep filesystem and allocation errors distinct. */
static int cli_share_asset(const char *relative, nb_buf *html)
{
	if (nb_buf_status(html))
		return nb_buf_status(html);
	char path[PATH_MAX];
	int rc = morph_data_find(path, sizeof(path), relative);
	if (rc)
		return rc;
	FILE *input = fopen(path, "rb");
	if (!input)
		return nb_status_from_errno(errno, NULL);
	char chunk[BUFSIZ];
	while (!rc) {
		errno = 0;
		size_t size = fread(chunk, 1, sizeof(chunk), input);
		int read_error =
			ferror(input) ? (errno ? nb_status_from_errno(errno, NULL) : NB_ERR_IO) : 0;
		rc = (nb_buf_append(html, chunk, size) ? 0 : NB_ERR_NOMEM);
		if (!rc && read_error)
			rc = (read_error);
		if (size < sizeof(chunk))
			break;
	}
	if (fclose(input) != 0 && !rc)
		rc = nb_status_from_errno(errno, NULL);
	return rc;
}

static int cli_share_header(nb_buf *html)
{
	nb_buf_append_cstr(
		html,
		"<!doctype html><html lang=en><head><meta charset=utf-8>"
		"<meta name=viewport content=\"width=device-width, initial-scale=1\">"
		"<meta http-equiv=Content-Security-Policy content=\"default-src 'none'; "
		"img-src data:; style-src 'unsafe-inline'; script-src 'nonce-morph-share'; "
		"base-uri 'none'; form-action 'none'\"><title>Shared from morph</title><style>");
	int rc = cli_share_asset("share/share.css", html);
	if (rc)
		return rc;
	nb_buf_append_cstr(html, "</style><script nonce=morph-share>");
	rc = cli_share_asset("share/share.js", html);
	if (rc)
		return rc;
	char path[PATH_MAX];
	rc = morph_data_find(path, sizeof(path), "morph-logo.png");
	if (rc)
		return rc;
	char *icon = NULL;
	rc = image_file_payload_copy(path, "data:image/png;base64,", MORPH_MEDIA_MAX_FILE_BYTES,
				     &icon);
	if (rc)
		return rc;
	nb_buf_printf(html,
		      "</script></head><body><main><header><div class=brand>"
		      "<img alt=\"\" src=\"%s\">morph</div>"
		      "<div class=themes role=group aria-label=\"Color theme\">"
		      "<button type=button data-set-theme=light aria-pressed=false "
		      "aria-label=\"Light theme\" title=\"Light theme\"><span "
		      "aria-hidden=true></span></button>"
		      "<button type=button data-set-theme=dark aria-pressed=false "
		      "aria-label=\"Dark theme\" title=\"Dark theme\"><span "
		      "aria-hidden=true></span></button>"
		      "</div></header>",
		      icon);
	free(icon);
	return nb_buf_status(html);
}

int cli_share_html(const cJSON *document, time_t shared_at, nb_buf *html)
{
	if (!document || !html ||
	    !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(document, "questions")) ||
	    !cli_share_text(document, "answer")[0])
		return NB_ERR_INVAL;
	if (nb_buf_status(html))
		return nb_buf_status(html);
	struct tm local;
	char stamp[80];
	if (!localtime_r(&shared_at, &local) ||
	    !strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC%z", &local))
		return NB_ERR_INVAL;
	int rc = cli_share_header(html);
	if (rc)
		return rc;
	rc = cli_share_questions(cJSON_GetObjectItemCaseSensitive(document, "questions"), html);
	if (rc)
		return rc;
	rc = cli_share_process(cJSON_GetObjectItemCaseSensitive(document, "steps"), html);
	if (rc)
		return rc;
	nb_buf_append_cstr(html, "<section><h2 class=label>MORPH</h2><article class=answer>");
	rc = cli_share_markdown(html, cli_share_text(document, "answer"));
	if (rc)
		return rc;
	nb_buf_append_cstr(html, "</article></section><footer><span>Shared from morph</span>"
				 "<span>Shared at <time>");
	rc = cli_share_escape(html, stamp);
	if (rc)
		return rc;
	nb_buf_append_cstr(html, "</time></span></footer></main></body></html>");
	return nb_buf_status(html);
}
