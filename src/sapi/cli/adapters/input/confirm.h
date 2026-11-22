#ifndef MORPH_CLI_ADAPTERS_INPUT_CONFIRM_H
#define MORPH_CLI_ADAPTERS_INPUT_CONFIRM_H

struct cli_application;

enum cli_input_confirm_result {
	CLI_INPUT_CONFIRM_DECLINED = 0,
	CLI_INPUT_CONFIRM_ACCEPTED = 1,
	CLI_INPUT_CONFIRM_NO_TERMINAL = 2,
	CLI_INPUT_CONFIRM_READ_ERROR = 3,
};

/* Plain output reads the controlling terminal. Active chat must use pages/confirmation. */
enum cli_input_confirm_result cli_input_confirm_yes_no(struct cli_application *ctx);
enum cli_input_confirm_result cli_input_confirm_phrase(struct cli_application *ctx,
						      const char *phrase);

#endif
