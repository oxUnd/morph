#include "sapi/cli/share.h"
#include "util/base64.h"
#include "util/data.h"
#include "util/utf8.h"
#include <morph_markdown_html.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *share_text(const cJSON *object, const char *key)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
	return cJSON_IsString(value) ? value->valuestring : "";
}

static int share_escape(morph_buf_t *html, const char *text)
{
	if (html->failed)
		return html->failed;
	char *safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		return -ENOMEM;
	for (const char *p = safe; *p; p++) {
		switch (*p) {
		case '&': morph_buf_puts(html, "&amp;"); break;
		case '<': morph_buf_puts(html, "&lt;"); break;
		case '>': morph_buf_puts(html, "&gt;"); break;
		case '"': morph_buf_puts(html, "&quot;"); break;
		case '\'': morph_buf_puts(html, "&#39;"); break;
		default: morph_buf_putc(html, *p); break;
		}
	}
	free(safe);
	return html->failed;
}

/* Export URL policy: omit image resources; permit only ordinary external links.
 * Fragment references are preserved by the Markdown exporter itself. */
static int share_resolve_resource(const struct morph_md_html_resource *resource,
				 const char **out_url, void *user_data)
{
	int *images_omitted = user_data;
	if (strcmp(resource->tag, "img") == 0 &&
	    strcmp(resource->attribute, "src") == 0) {
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

static int share_markdown(morph_buf_t *html, const char *text)
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
		.resolve_resource = share_resolve_resource,
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
		if (rc != -ENOMEM && rc != -EINVAL)
			rc = (MORPH_ERR_FORMAT);
	} else {
		rc = morph_buf_puts(html, fragment);
		if (!rc && images_omitted)
			rc = morph_buf_puts(html, "<p class=note>Images omitted from this export.</p>");
	}
	morph_md_free(fragment);
	morph_md_doc_release(document);
	morph_md_engine_destroy(engine);
	return rc;
}

static int share_questions(const cJSON *questions, morph_buf_t *html)
{
	morph_buf_puts(html, "<section><h2 class=label>YOU</h2>");
	const cJSON *question;
	cJSON_ArrayForEach(question, questions) {
		morph_buf_puts(html, "<div class=question>");
		int rc = share_escape(html, cJSON_IsString(question) ? question->valuestring : "");
		if (rc)
			return rc;
		morph_buf_puts(html, "</div>");
	}
	morph_buf_puts(html, "</section>");
	return html->failed;
}

static int share_step_html(const cJSON *step, morph_buf_t *html)
{
	morph_buf_puts(html, "<li><div class=step-title>");
	int rc = share_escape(html, share_text(step, "name"));
	if (rc)
		return rc;
	morph_buf_puts(html, "<span class=status>");
	rc = share_escape(html, share_text(step, "status"));
	if (rc)
		return rc;
	morph_buf_puts(html, "</span></div><details class=tool-output>"
		"<summary>View result</summary><pre>");
	rc = share_escape(html, share_text(step, "content"));
	if (rc)
		return rc;
	morph_buf_puts(html, "</pre>");
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(step, "truncated")))
		morph_buf_puts(html, "<p class=note>This result was shortened "
			"before it was stored.</p>");
	morph_buf_puts(html, "</details></li>");
	return html->failed;
}

static int share_process(const cJSON *steps, morph_buf_t *html)
{
	int count = cJSON_GetArraySize(steps);
	if (count == 0)
		return 0;
	morph_buf_printf(html, "<details class=process><summary>Execution process · "
		"%d step%s</summary><ol class=steps>", count, count == 1 ? "" : "s");
	const cJSON *step;
	cJSON_ArrayForEach(step, steps) {
		int rc = share_step_html(step, html);
		if (rc)
			return rc;
	}
	morph_buf_puts(html, "</ol></details>");
	return html->failed;
}

/* Stream an asset into the document; keep filesystem and allocation errors distinct. */
static int share_asset(const char *relative, morph_buf_t *html)
{
	if (html->failed)
		return html->failed;
	char path[PATH_MAX];
	int rc = morph_data_find(path, sizeof(path), relative);
	if (rc)
		return rc;
	FILE *input = fopen(path, "rb");
	if (!input)
		return morph_error_from_errno(errno);
	char chunk[BUFSIZ];
	while (!rc) {
		errno = 0;
		size_t size = fread(chunk, 1, sizeof(chunk), input);
		int read_error = ferror(input) ? (errno ? -errno : -EIO) : 0;
		rc = morph_buf_append(html, chunk, size);
		if (!rc && read_error)
			rc = (read_error);
		if (size < sizeof(chunk))
			break;
	}
	if (fclose(input) != 0 && !rc)
		rc = morph_error_from_errno(errno);
	return rc;
}

static int share_header(morph_buf_t *html)
{
	morph_buf_puts(html, "<!doctype html><html lang=en><head><meta charset=utf-8>"
		"<meta name=viewport content=\"width=device-width, initial-scale=1\">"
		"<meta http-equiv=Content-Security-Policy content=\"default-src 'none'; "
		"img-src data:; style-src 'unsafe-inline'; script-src 'nonce-morph-share'; "
		"base-uri 'none'; form-action 'none'\"><title>Shared from morph</title><style>");
	int rc = share_asset("share/share.css", html);
	if (rc)
		return rc;
	morph_buf_puts(html, "</style><script nonce=morph-share>");
	rc = share_asset("share/share.js", html);
	if (rc)
		return rc;
	char path[PATH_MAX];
	rc = morph_data_find(path, sizeof(path), "morph-logo.png");
	if (rc)
		return rc;
	char *icon = NULL;
	rc = base64_encode_file_prefixed(path, "data:image/png;base64,",
		MORPH_MEDIA_MAX_FILE_BYTES, &icon);
	if (rc)
		return rc;
	morph_buf_printf(html, "</script></head><body><main><header><div class=brand>"
		"<img alt=\"\" src=\"%s\">morph</div>"
		"<div class=themes role=group aria-label=\"Color theme\">"
		"<button type=button data-set-theme=light aria-pressed=false "
		"aria-label=\"Light theme\" title=\"Light theme\"><span aria-hidden=true></span></button>"
		"<button type=button data-set-theme=dark aria-pressed=false "
		"aria-label=\"Dark theme\" title=\"Dark theme\"><span aria-hidden=true></span></button>"
		"</div></header>", icon);
	free(icon);
	return html->failed;
}

int cli_share_html(const cJSON *document, time_t shared_at, morph_buf_t *html)
{
	if (!document || !html ||
	    !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(document, "questions")) ||
	    !share_text(document, "answer")[0])
		return -EINVAL;
	if (html->failed)
		return html->failed;
	struct tm local;
	char stamp[80];
	if (!localtime_r(&shared_at, &local) ||
	    !strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC%z", &local))
		return -EINVAL;
	int rc = share_header(html);
	if (rc)
		return rc;
	rc = share_questions(cJSON_GetObjectItemCaseSensitive(document, "questions"), html);
	if (rc)
		return rc;
	rc = share_process(cJSON_GetObjectItemCaseSensitive(document, "steps"), html);
	if (rc)
		return rc;
	morph_buf_puts(html, "<section><h2 class=label>MORPH</h2><article class=answer>");
	rc = share_markdown(html, share_text(document, "answer"));
	if (rc)
		return rc;
	morph_buf_puts(html, "</article></section><footer><span>Shared from morph</span>"
		"<span>Shared at <time>");
	rc = share_escape(html, stamp);
	if (rc)
		return rc;
	morph_buf_puts(html, "</time></span></footer></main></body></html>");
	return html->failed;
}
