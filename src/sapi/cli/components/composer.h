#ifndef MORPH_CLI_COMPONENTS_COMPOSER_H
#define MORPH_CLI_COMPONENTS_COMPOSER_H

#include <nothing/tui/view.h>
#include <stdint.h>

struct cli_application;

/* Both returned views are borrowed for their controller's lifetime. */
struct nt_view *cli_composer_add_borrow(struct nt_builder *, struct nt_view *parent,
					struct cli_application *, struct nt_view **editor_borrow);
int cli_composer_update(struct nt_view *, int64_t now_ms);
/* Preserve editable rows before shrinking the component's own spacing.
 * A preceding history separator replaces this component's leading margin. */
int cli_composer_fit(struct nt_view *, int width, int height, int preceding_separator);

#endif
