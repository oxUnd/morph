#ifndef MORPH_REACT_INTERNAL_H
#define MORPH_REACT_INTERNAL_H

#include "react.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Instance-local platform seam. Returns a positive pthread error; a failed join
 * must leave the worker joinable so the owner can drain it. Borrowed through drain. */
struct react_thread_ops {
	int (*join)(pthread_t thread, void *user_data);
	void *user_data;
};

#ifdef __cplusplus
}
#endif
#endif
