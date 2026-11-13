#ifndef MORPH_CLI_ADAPTERS_INPUT_CONFIRM_H
#define MORPH_CLI_ADAPTERS_INPUT_CONFIRM_H

enum cli_input_confirm_result {
	CLI_INPUT_CONFIRM_DECLINED = 0,
	CLI_INPUT_CONFIRM_ACCEPTED = 1,
	CLI_INPUT_CONFIRM_NO_TERMINAL = 2,
	CLI_INPUT_CONFIRM_READ_ERROR = 3,
};

/* Reads one answer from the controlling terminal. */
enum cli_input_confirm_result cli_input_confirm_yes_no(void);
enum cli_input_confirm_result cli_input_confirm_phrase(const char *phrase);

#endif
