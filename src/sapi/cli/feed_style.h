#ifndef CLI_FEED_STYLE_H
#define CLI_FEED_STYLE_H

/* One symbol column and one space precede feed content. */
#define CLI_FEED_DONE "◯"
#define CLI_FEED_RUNNING "◉"
#define CLI_FEED_FAILED "⊗"
#define CLI_FEED_SYSTEM "◇"
#define CLI_FEED_PENDING "○"
#define CLI_FEED_QUESTION "?"

/* Keep editable input and submitted user messages visually consistent. */
#define CLI_COMMAND_COLOR "\033[38;5;141m"
#define CLI_INPUT_BACKGROUND "\033[48;5;234m"
#define CLI_INPUT_MARKER_COLOR "\033[38;5;245m"

#endif
