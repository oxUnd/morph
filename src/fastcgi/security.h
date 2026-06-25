/* security.h — FastCGI auth helpers */
#ifndef MORPH_FCGI_SECURITY_H
#define MORPH_FCGI_SECURITY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int fcgi_random_id(const char *prefix, char *out, size_t out_size);
int fcgi_password_hash(const char *password, char *out, size_t out_size);
int fcgi_password_verify(const char *password, const char *encoded);
int fcgi_basic_decode(const char *auth_hdr, char *user, size_t user_size,
		      char *pass, size_t pass_size);

#ifdef __cplusplus
}
#endif

#endif
