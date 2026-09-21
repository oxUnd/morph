#include "sapi/cli/cli.h"
#include "sapi/cli/setup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) return 2;
	cli_set_color_enabled(argc < 3 || strcmp(argv[2], "--no-color") != 0);
	struct termios before;
	struct termios after;
	if (tcgetattr(STDIN_FILENO, &before) != 0) return 2;
	for (int i = 0; i < 32; i++)
		printf("SHELL_HISTORY_%02d\n", i);
	fflush(stdout);
	int rc = cli_setup(argv[1], stdin, stdout);
	if (tcgetattr(STDIN_FILENO, &after) != 0) return 2;
	/* macOS sets PENDIN when returning to canonical input. */
#ifdef PENDIN
	after.c_lflag &= (tcflag_t)~PENDIN;
	before.c_lflag &= (tcflag_t)~PENDIN;
#endif
	int restored = before.c_iflag == after.c_iflag &&
		before.c_oflag == after.c_oflag && before.c_lflag == after.c_lflag &&
		memcmp(before.c_cc, after.c_cc, sizeof(before.c_cc)) == 0;
	printf("SETUP_RESULT=%d RAW_RESTORED=%d SESSION_KEY_READY=%d\n", rc,
		restored, getenv("OPENAI_API_KEY") != NULL);
	return restored ? 0 : 2;
}
