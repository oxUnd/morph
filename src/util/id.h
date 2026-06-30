#ifndef MORPH_ID_H
#define MORPH_ID_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int morph_random_bytes(unsigned char *buf, size_t len);
int morph_random_id(const char *prefix, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
