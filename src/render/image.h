#ifndef IMAGE_RENDER_H
#define IMAGE_RENDER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int image_render_terminal(const char *path);
int image_terminal_protocol_available(void);

/* Detect image format from magic bytes: 100=PNG, 101=JPEG, 0=unknown */
int image_detect_fmt(const unsigned char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
