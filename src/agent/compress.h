#ifndef COMPRESS_H
#define COMPRESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/context.h"
#include <errno.h>

int compress_sliding_window(struct message_list **head, int keep_rounds,
			    struct compress_result *result);
int compress_react_trace(struct message_list **head,
			 struct compress_result *result);
struct key_info *extract_key_info(struct message_list *head);
void key_info_free(struct key_info *head);

#ifdef __cplusplus
}
#endif

#endif