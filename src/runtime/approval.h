#ifndef MORPH_RUNTIME_APPROVAL_H
#define MORPH_RUNTIME_APPROVAL_H

#include "permission/permission.h"

struct runtime;
int runtime_approval_init(struct runtime *runtime);
int runtime_approval_parse(const char *text, struct permission_review *out);

#endif
