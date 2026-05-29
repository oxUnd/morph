#ifndef MORPH_LATEX_UNICODE_H
#define MORPH_LATEX_UNICODE_H

#include <stddef.h>

enum latex_render_flags {
	LATEX_INLINE  = 0,
	LATEX_DISPLAY = 1 << 0,
};

int latex_to_unicode(const char *latex, size_t len,
		    char *out, size_t out_cap,
		    int flags);

#endif
