#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_MAX_KEY_LEN 256
#define CONFIG_MAX_VAL_LEN 2048

struct config_general {
	char default_session[256];
	char output_dir[512];
	char log_level[16];
	char log_file[512];
};

struct config_model_entry {
	char provider[64];
	char model[128];
	char api_base[256];
	char api_key_env[64];
	char api_key[256];
	int context_limit;
	int max_tokens;
	int timeout_seconds;
	int poll_interval_seconds;
	int poll_timeout_seconds;
};

struct config_models {
	struct config_model_entry text;
	struct config_model_entry image;
	struct config_model_entry video;
};

#define DISABLED_TOOLS_MAX 32
#define DISABLED_TOOL_NAME_MAX 64
#define HITL_TOOLS_MAX 32
#define HITL_TOOL_NAME_MAX 64
#define BASH_EXEC_ALLOW_MAX 32
#define BASH_EXEC_COMMAND_MAX 1024
#define BASH_EXEC_CWD_MAX 512

struct config_react {
	int max_iterations;
	int step_timeout_seconds;
	int tool_max_retries;
	int guardrail_enabled;
	int guardrail_max_retries;
	int guardrail_max_empty_rounds;
	char disabled_tools[DISABLED_TOOLS_MAX][DISABLED_TOOL_NAME_MAX];
	int disabled_tools_count;
	int hitl_enabled;
	char hitl_tools[HITL_TOOLS_MAX][HITL_TOOL_NAME_MAX];
	int hitl_tools_count;
	int hitl_auto_approve_readonly;
	int bash_exec_enabled;
	int bash_exec_default_timeout;
	char bash_exec_allowed_commands[BASH_EXEC_ALLOW_MAX][BASH_EXEC_COMMAND_MAX];
	int bash_exec_allowed_commands_count;
	char bash_exec_allowed_cwds[BASH_EXEC_ALLOW_MAX][BASH_EXEC_CWD_MAX];
	int bash_exec_allowed_cwds_count;
};

struct config_context {
	double summarize_threshold_ratio;
	double compress_target_ratio;
	int keep_recent_rounds;
};

struct config_memory {
	int enabled;
	int hot_path_enabled;
	int cold_path_enabled;
	int max_facts;
	int max_episodes;
	int max_procedures;
	int max_context_chars;
};

struct config_render {
	char prefer_image_protocol[16];
	char mpv_args[256];
};

struct config_ext {
	char dir[512];
	int default_max_memory_mb;
	int default_max_cpu_seconds;
};

struct config_prompt {
	char system_prompt_file[512];
	char system_prompt_dir[512];
};

struct config_skill {
	char dir[512];
};

#define MCP_SERVER_MAX 32
#define MCP_SERVER_NAME_MAX 64
#define MCP_CMD_ARGS_MAX 32
#define MCP_CMD_ARG_LEN_MAX 256
#define MCP_ENV_MAX 16
#define MCP_ENV_VAL_MAX 512

struct config_mcp_server {
	char name[MCP_SERVER_NAME_MAX];
	char transport[16];
	/* stdio */
	char command[256];
	char args[MCP_CMD_ARGS_MAX][MCP_CMD_ARG_LEN_MAX];
	int args_count;
	char env_keys[MCP_ENV_MAX][64];
	char env_vals[MCP_ENV_MAX][MCP_ENV_VAL_MAX];
	int env_count;
	/* http */
	char http_url[512];
	char http_auth_token_env[64];
	/* startup */
	int auto_connect;
	int connect_timeout;
};

struct config_mcp {
	struct config_mcp_server servers[MCP_SERVER_MAX];
	int server_count;
};

struct config {
	struct config_general general;
	struct config_models models;
	struct config_react react;
	struct config_context context;
	struct config_memory memory;
	struct config_render render;
	struct config_ext ext;
	struct config_prompt prompt;
	struct config_skill skill;
	struct config_mcp mcp;
};

int config_load(struct config *cfg, const char *path);
void config_set_defaults(struct config *cfg);
void config_print(const struct config *cfg);

#ifdef __cplusplus
}
#endif

#endif
