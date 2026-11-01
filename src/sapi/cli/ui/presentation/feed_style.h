#ifndef MORPH_CLI_UI_PRESENTATION_FEED_STYLE_H
#define MORPH_CLI_UI_PRESENTATION_FEED_STYLE_H

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

/* Large markers preserve state through shape as well as color. */
#define CLI_FEED_DONE "●"
#define CLI_FEED_RUNNING "◉"
#define CLI_FEED_FAILED "⊗"
#define CLI_FEED_SYSTEM "◇"
#define CLI_FEED_PENDING "◯"
#define CLI_FEED_QUESTION "?"

/* Keep editable input and submitted user messages visually consistent. */
#define CLI_COMMAND_COLOR "\033[38;5;141m"
#define CLI_INPUT_BACKGROUND "\033[48;5;234m"
#define CLI_INPUT_MARKER_COLOR "\033[38;5;245m"

#endif
