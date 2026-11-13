#ifndef MORPH_EXT_READ_LINES_H
#define MORPH_EXT_READ_LINES_H

/* The result uses malloc ownership; the caller frees it, including on errors.
 * Returns NB_OK or a negative nbase status. Allocation failure may leave it NULL. */
int ext_run(const char *args_json, char **result_json);

#endif
