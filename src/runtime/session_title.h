#ifndef MORPH_RUNTIME_SESSION_TITLE_H
#define MORPH_RUNTIME_SESSION_TITLE_H

#define SESSION_TITLE_MAX_CHARS 36
#define SESSION_TITLE_PROMPT_MAX_BYTES 960

/* Strict JSON input; returns a normalized, malloc-owned title or NULL. */
char *runtime_title_parse(const char *response);

#endif
