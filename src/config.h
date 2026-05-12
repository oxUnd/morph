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
	int timeout_seconds;
	int poll_interval_seconds;
	int poll_timeout_seconds;
};

struct config_models {
	struct config_model_entry text;
	struct config_model_entry image;
	struct config_model_entry video;
};

struct config_react {
	int max_iterations;
	int step_timeout_seconds;
	int tool_max_retries;
};

struct config_context {
	double summarize_threshold_ratio;
	double compress_target_ratio;
	int keep_recent_rounds;
};

struct config_render {
	char prefer_image_protocol[16];
	char mpv_args[256];
};

struct config_skill {
	char dir[512];
	int default_max_memory_mb;
	int default_max_cpu_seconds;
};

struct config {
	struct config_general general;
	struct config_models models;
	struct config_react react;
	struct config_context context;
	struct config_render render;
	struct config_skill skill;
};

int config_load(struct config *cfg, const char *path);
void config_set_defaults(struct config *cfg);
void config_print(const struct config *cfg);

#ifdef __cplusplus
}
#endif

#endif