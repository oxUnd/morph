#ifndef MARKDOWN_H
#define MARKDOWN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

void markdown_render_ansi(const char *md);
size_t markdown_render_ansi_to_buf(const char *md, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif