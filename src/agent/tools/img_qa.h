#ifndef IMG_QA_H
#define IMG_QA_H

#ifdef __cplusplus
extern "C" {
#endif

struct tool_registry;
struct tool_context;
struct model;

int img_qa_init(struct tool_registry *reg, struct model *llm,
		struct tool_context *tctx);

#ifdef __cplusplus
}
#endif

#endif
