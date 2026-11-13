#ifndef MORPH_CLI_SERVICES_TURN_H
#define MORPH_CLI_SERVICES_TURN_H
#include <stdint.h>
struct cli_application;

void cli_turn_prepare(struct cli_application *ctx);
int cli_turn_execute_prompt(struct cli_application *, const char *input);
void cli_turn_record_media_credits(struct cli_application *ctx, const char *kind,
				   int64_t image_units, int64_t video_seconds, const char *provider,
				   const char *model, const char *metadata_json);
void cli_turn_record_input_images(struct cli_application *ctx, const char *input);
void cli_turn_begin(struct cli_application *ctx);
void cli_turn_finish(struct cli_application *ctx, int turn_rc);

#endif
