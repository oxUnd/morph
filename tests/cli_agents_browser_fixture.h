#ifndef MORPH_TEST_CLI_AGENTS_BROWSER_FIXTURE_H
#define MORPH_TEST_CLI_AGENTS_BROWSER_FIXTURE_H

#include "sapi/cli/components/agents_browser.h"

int test_agents_browser_render(nb_buf *output, struct cli_agents_browser_state *state,
			       const struct cli_agents_browser_data *data, int columns, int rows,
			       int styled);

#endif
