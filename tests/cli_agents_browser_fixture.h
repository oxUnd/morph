#ifndef MORPH_TEST_CLI_AGENTS_BROWSER_FIXTURE_H
#define MORPH_TEST_CLI_AGENTS_BROWSER_FIXTURE_H

#include "sapi/cli/components/agents_browser.h"
#include <nb/buf.h>

typedef int (*test_agents_browser_refresh)(struct nt_view *, struct cli_agents_browser_state *,
					  const struct cli_agents_browser_data *, int styled);

int test_agents_browser_render(nb_buf *output, struct cli_agents_browser_state *state,
			       const struct cli_agents_browser_data *data, int columns, int rows,
			       int styled, test_agents_browser_refresh refresh);

#endif
