#ifndef MORPH_RUNTIME_MODEL_H
#define MORPH_RUNTIME_MODEL_H

#ifdef __cplusplus
extern "C" {
#endif

struct runtime;
/* Ultra controls proactive collaboration without changing provider parameters. */
int runtime_model_get_ultra(struct runtime *runtime, int *ultra);
/* Saves the selection before publishing it; requires an idle runtime. */
int runtime_model_set_ultra(struct runtime *runtime, int ultra);

#ifdef __cplusplus
}
#endif
#endif
