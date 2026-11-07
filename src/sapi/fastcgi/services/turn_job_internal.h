#ifndef MORPH_FCGI_SERVICES_TURN_JOB_INTERNAL_H
#define MORPH_FCGI_SERVICES_TURN_JOB_INTERNAL_H

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

struct fcgi_bridge;
struct session_store;
struct fcgi_turn_ticket;

struct fcgi_turn_job {
	struct fcgi_bridge *bridge;
	struct session_store *store;
	char session_id[64];
	char user_id[64];
	char turn_id[64];
	char *input;
	char last_tool[128];
	char last_tool_call_id[128];
	struct fcgi_turn_artifact {
		char path[PATH_MAX];
		char id[64];
		char kind[16];
		char mime[64];
		char filename[128];
		char url[128];
		int64_t size_bytes;
	} artifacts[32];
	int artifacts_count;
	int final_sent;
	char action_type[32];
	char *action_payload;
	char pending_type[32];
	char *pending_payload;
	unsigned long long ask_sequence;
	struct fcgi_turn_ticket *ticket;
	int queue_position;
	int64_t enqueued_ms;
	pthread_t heartbeat_thread;
	atomic_int heartbeat_stop;
	int heartbeat_started;
	char pending_thought[2048];
	size_t pending_thought_len;
	int64_t pending_thought_ms;
	char pending_reasoning[2048];
	size_t pending_reasoning_len;
	int64_t pending_reasoning_ms;
	char pending_tool_stream[2048];
	size_t pending_tool_stream_len;
	int64_t pending_tool_stream_ms;
	char pending_tool_name[128];
	char pending_tool_kind[32];
	char pending_tool_call_id[128];
};

#endif
