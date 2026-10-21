#ifndef MORPH_NAME_H
#define MORPH_NAME_H

#ifdef __cplusplus
extern "C" {
#endif

/* ASCII letters and digits, plus the explicitly allowed punctuation. */
int morph_name_char_valid(char c, const char *punctuation);
/* Names must be non-empty. Length and prefix rules belong to the caller. */
int morph_name_valid(const char *name, const char *punctuation);
/* Environment variables use letters or '_' first, then also allow digits. */
int morph_env_name_valid(const char *name);

#ifdef __cplusplus
}
#endif

#endif
