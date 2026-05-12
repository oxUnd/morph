#ifndef LOADER_H
#define LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

struct skill;

int skill_load_so(struct skill *sk, const char *path);
int skill_load_exec(struct skill *sk, const char *path);
void skill_unload_so(struct skill *sk);

#ifdef __cplusplus
}
#endif

#endif