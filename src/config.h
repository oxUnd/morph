#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>

#define SUB_AGENT_TOOL_NAME_MAX 64

#define CONFIG_MAX_KEY_LEN 256
#define CONFIG_MAX_VAL_LEN 2048

struct config_general {
	char default_session[256];
	char output_dir[PATH_MAX];
	char log_level[16];
	char log_file[PATH_MAX];
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

#define CREDIT_PRICE_MAX 32
#define CREDIT_KIND_MAX 32

struct config_credit_price {
	char provider[64];
	char model[128];
	char kind[CREDIT_KIND_MAX];
	double input_per_million;
	double output_per_million;
	double image_unit_per_million;
	double video_second_per_million;
};

struct config_credits {
	int daily_limit;
	char currency[8];
	double cost_to_credit_coef;
	double input_token_credit_coef;
	double output_token_credit_coef;
	double image_unit_credit_coef;
	double video_second_credit_coef;
	struct config_credit_price prices[CREDIT_PRICE_MAX];
	int price_count;
};

#define DISABLED_TOOLS_MAX 32
#define DISABLED_TOOL_NAME_MAX 64
#define HITL_TOOLS_MAX 32
#define HITL_TOOL_NAME_MAX 64
#define BASH_EXEC_ALLOW_MAX 32
#define BASH_EXEC_COMMAND_MAX 1024
#define BASH_EXEC_CWD_MAX PATH_MAX
#define GUARDRAIL_DISABLED_RULES_MAX 16
#define GUARDRAIL_LLM_RULES_MAX 8
#define GUARDRAIL_EXT_RULES_MAX 8
#define CFG_GR_NAME_MAX 64
#define CFG_GR_DESC_MAX 1024
#define CFG_GR_ACTION_MAX 512
#define CFG_GR_EXT_ENTRY_MAX PATH_MAX

struct config_guardrail_llm_rule {
	char name[CFG_GR_NAME_MAX];
	char hook[32];
	char description[CFG_GR_DESC_MAX];
	char action_text[CFG_GR_ACTION_MAX];
};

struct config_guardrail_ext_rule {
	char name[CFG_GR_NAME_MAX];
	char hook[32];
	char ext_type[8];
	char ext_entry[CFG_GR_EXT_ENTRY_MAX];
	char action_text[CFG_GR_ACTION_MAX];
};

struct config_react {
	int max_iterations;
	int step_timeout_seconds;
	int tool_max_retries;
	int guardrail_enabled;
	int guardrail_max_retries;
	int guardrail_max_empty_rounds;
	char guardrail_disabled_rules[GUARDRAIL_DISABLED_RULES_MAX][CFG_GR_NAME_MAX];
	int guardrail_disabled_rule_count;
	struct config_guardrail_llm_rule guardrail_llm_rules[GUARDRAIL_LLM_RULES_MAX];
	int guardrail_llm_rule_count;
	struct config_guardrail_ext_rule guardrail_ext_rules[GUARDRAIL_EXT_RULES_MAX];
	int guardrail_ext_rule_count;
	char guardrail_llm_model[128];
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
	int llm_extract_enabled;
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
	char dir[PATH_MAX];
	int default_max_memory_mb;
	int default_max_cpu_seconds;
};

struct config_prompt {
	char system_prompt_file[PATH_MAX];
	char system_prompt_dir[PATH_MAX];
};

struct config_skill {
	char dir[PATH_MAX];
};

#define MCP_SERVER_MAX 32
#define MCP_SERVER_NAME_MAX 64
#define MCP_CMD_ARGS_MAX 32
#define MCP_CMD_ARG_LEN_MAX 256
#define MCP_ENV_MAX 16
#define MCP_ENV_VAL_MAX PATH_MAX

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
	char http_url[PATH_MAX];
	char http_auth_token_env[64];
	/* startup */
	int auto_connect;
	int connect_timeout;
};

struct config_mcp {
	struct config_mcp_server servers[MCP_SERVER_MAX];
	int server_count;
};

#define DYNAMIC_TOOL_CAP_MAX 16
#define DYNAMIC_TOOL_CAP_LEN_MAX 32
#define DYNAMIC_TOOL_ALLOW_MAX 32
#define DYNAMIC_TOOL_ALLOW_LEN_MAX PATH_MAX

struct config_dynamic_tool_profile {
	char default_capabilities[DYNAMIC_TOOL_CAP_MAX][DYNAMIC_TOOL_CAP_LEN_MAX];
	int default_capabilities_count;
	char allowed_read_paths[DYNAMIC_TOOL_ALLOW_MAX][DYNAMIC_TOOL_ALLOW_LEN_MAX];
	int allowed_read_paths_count;
	char allowed_write_paths[DYNAMIC_TOOL_ALLOW_MAX][DYNAMIC_TOOL_ALLOW_LEN_MAX];
	int allowed_write_paths_count;
	char allowed_commands[DYNAMIC_TOOL_ALLOW_MAX][DYNAMIC_TOOL_ALLOW_LEN_MAX];
	int allowed_commands_count;
	char allowed_network[DYNAMIC_TOOL_ALLOW_MAX][DYNAMIC_TOOL_ALLOW_LEN_MAX];
	int allowed_network_count;
};

struct config_dynamic_tools {
	int enabled;
	char runtime[16];
	char mode[16];
	int mode_explicit;
	char session_dir[PATH_MAX];
	char persistent_dir[PATH_MAX];
	char default_lifetime[16];
	int create_requires_approval;
	int promote_requires_approval;
	int max_source_bytes;
	int default_timeout_seconds;
	int default_max_output_bytes;
	struct config_dynamic_tool_profile local;
	struct config_dynamic_tool_profile server;
};

#define SUB_AGENT_MAX 16
#define SUB_AGENT_NAME_MAX 64
#define SUB_AGENT_TOOL_MAX 32

enum sub_agent_context_policy {
	SUB_AGENT_CTX_FULL,
	SUB_AGENT_CTX_SUMMARY,
	SUB_AGENT_CTX_TASK_ONLY
};

enum sub_agent_merge_strategy {
	SUB_AGENT_MERGE_SYNTHESIZE,
	SUB_AGENT_MERGE_CONCAT,
	SUB_AGENT_MERGE_RAW
};

struct config_sub_agent {
	char name[SUB_AGENT_NAME_MAX];
	char description[256];
	char system_prompt_file[PATH_MAX];
	char model[64];
	int max_iterations;
	char allowed_tools[SUB_AGENT_TOOL_MAX][SUB_AGENT_TOOL_NAME_MAX];
	int allowed_tools_count;
	char disabled_tools[SUB_AGENT_TOOL_MAX][SUB_AGENT_TOOL_NAME_MAX];
	int disabled_tools_count;
	enum sub_agent_context_policy context_policy;
	enum sub_agent_merge_strategy merge_strategy;
	char *output_schema;
};

struct config_sub_agents {
	struct config_sub_agent entries[SUB_AGENT_MAX];
	int count;
};

struct config {
	struct config_general general;
	struct config_models models;
	struct config_credits credits;
	struct config_react react;
	struct config_context context;
	struct config_memory memory;
	struct config_render render;
	struct config_ext ext;
	struct config_prompt prompt;
	struct config_skill skill;
	struct config_mcp mcp;
	struct config_dynamic_tools dynamic_tools;
	struct config_sub_agents sub_agents;
};

int config_load(struct config *cfg, const char *path);
int config_load_sub_agents(struct config *cfg, const char *path);
void config_set_defaults(struct config *cfg);
void config_print(const struct config *cfg);

#ifdef __cplusplus
}
#endif

#endif
