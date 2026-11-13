#ifndef MORPH_CLI_PRESENTATION_STYLE_H
#define MORPH_CLI_PRESENTATION_STYLE_H

#include <morph/tui/terminal/status.h>
#include <stdint.h>

/* Shared right gutter for conversation text and compact tool rows. */
#define CLI_CONTENT_RIGHT_PADDING 2u

#define ANSI_BOLD "\033[1m"
#define ANSI_DIM "\033[2m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN "\033[36m"
#define ANSI_RESET "\033[0m"

#ifdef HAVE_READLINE
#define CLI_RL_IGNORE_START "\001"
#define CLI_RL_IGNORE_END "\002"
#else
#define CLI_RL_IGNORE_START ""
#define CLI_RL_IGNORE_END ""
#endif

/* Canonical CLI status shapes and styles; see AGENTS.md. */
#define CLI_FEED_DONE MORPH_TERMINAL_STATUS_DONE
#define CLI_FEED_RUNNING MORPH_TERMINAL_STATUS_RUNNING
#define CLI_FEED_FAILED MORPH_TERMINAL_STATUS_FAILED
#define CLI_FEED_SYSTEM MORPH_TERMINAL_STATUS_SYSTEM
#define CLI_FEED_PENDING MORPH_TERMINAL_STATUS_PENDING
#define CLI_FEED_QUESTION MORPH_TERMINAL_STATUS_QUESTION
#define CLI_FEED_CANCELLED MORPH_TERMINAL_STATUS_CANCELLED
#define CLI_FEED_WARNING MORPH_TERMINAL_STATUS_WARNING
#define CLI_FEED_MESSAGE MORPH_TERMINAL_STATUS_DONE
#define CLI_FEED_FAILED_STYLE ANSI_BOLD ANSI_RED
#define CLI_FEED_QUESTION_STYLE ANSI_BOLD ANSI_YELLOW

#define CLI_STYLE_RUNNING_PULSE_MS 500

/* Use a fixed-width, visible marker rather than terminal-dependent SGR blink. */
static inline const char *cli_style_running_color(int64_t now_ms)
{
	return (now_ms / CLI_STYLE_RUNNING_PULSE_MS) % 2 ? ANSI_DIM ANSI_CYAN : ANSI_CYAN;
}

static inline int cli_style_running_interval_ms(int64_t now_ms)
{
	return CLI_STYLE_RUNNING_PULSE_MS - (int)(now_ms % CLI_STYLE_RUNNING_PULSE_MS);
}

/* Keep editable input and submitted user messages visually consistent. */
#define CLI_INPUT_BACKGROUND_INDEX 237u
#define CLI_INPUT_MARKER_INDEX 245u
#define CLI_INPUT_ULTRA_INDEX 141u
#define CLI_COMMAND_COLOR "\033[38;5;141m"
#define CLI_INPUT_BACKGROUND "\033[48;5;237m"
#define CLI_INPUT_MARKER_COLOR "\033[38;5;245m"
#define CLI_INPUT_ULTRA_COLOR "\033[1;38;5;141m"

#endif
