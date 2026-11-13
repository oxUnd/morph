#ifndef NB_CONTAINER_INTERNAL_H
#define NB_CONTAINER_INTERNAL_H

#include "nb/base.h"

/* Integer addresses avoid ordering/subtracting pointers to unrelated objects.
 * Callers only rebase spans wholly contained in initialized storage. */
static inline bool nb_span_offset(const void *base, size_t bytes,
                                  const void *ptr, size_t len, size_t *offset)
{
    uintptr_t start = (uintptr_t)base;
    uintptr_t address = (uintptr_t)ptr;
    if (!base || !ptr || address < start || address - start > bytes) {
        return false;
    }
    size_t at = (size_t)(address - start);
    if (len > bytes - at) {
        return false;
    }
    *offset = at;
    return true;
}

#endif
