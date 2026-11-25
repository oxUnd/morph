#ifndef MORPH_CLI_ADAPTERS_INPUT_FOCUS_H
#define MORPH_CLI_ADAPTERS_INPUT_FOCUS_H

struct nt_application;
struct nt_event;

/* Unhandled text/paste targets the sole available editor in the active scope,
 * or the sole editor in the focused split pane when the scope has several. */
int cli_input_focus_install(struct nt_application *);
/* Page capture handlers may also forward explicit editing keys. Native editor
 * handlers retain selection, paste sanitization, undo and change events. */
int cli_input_focus_forward(struct nt_application *, const struct nt_event *);

#endif
