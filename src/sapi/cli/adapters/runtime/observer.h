#ifndef MORPH_CLI_ADAPTERS_RUNTIME_OBSERVER_H
#define MORPH_CLI_ADAPTERS_RUNTIME_OBSERVER_H
struct model_usage;
void cli_runtime_observer_pause(void *data);
void cli_runtime_observer_resume(void *data);
void cli_runtime_observer_usage(const struct model_usage *, void *data);
#endif
