#ifndef AGENT_UI_H
#define AGENT_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Agent UI parses the legacy <m:...> tag syntax into a normalized IR.
 * The returned JSON string is heap allocated and must be freed by the caller.
 */
char *agent_ui_parse_tags_json(const char *input);

/*
 * Normalize common model-produced Markdown deviations before rendering.
 * The returned string is heap allocated and must be freed by the caller.
 */
char *agent_ui_normalize_markdown(const char *input);

#ifdef __cplusplus
}
#endif

#endif
