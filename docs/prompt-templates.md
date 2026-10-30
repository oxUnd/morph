# Prompt templates

## Scope and decisions

This change implements the template foundation before adding role presets.
The renderer is a general-purpose prompt module: callers supply template text
or a file bundle and explicit typed bindings, and receive ordinary UTF-8 text.
It is not specific to setup, system prompts, any model, or any front end.
Setup, main agents, subagents, compaction, and other prompt producers can use
the same API and choose their own rendering lifecycle and context.

The implementation lives in `src/prompt/template.c`, with its API in
`src/prompt/template.h`, in the `morph-prompt` static library. It depends only on
`morph-util`; `morph-agent` links it publicly for agent and frontend consumers.
This phase implements and tests the reusable API. It does not automatically
reinterpret existing prompt files or change setup/runtime behavior. There is no
role picker, role prompt, permission grant, model requirement, new configuration
key, or new CLI command in this change.

This is a small, explicitly versioned Mustache-style subset, not a Mustache or
Jinja compatibility implementation. It adds no dependency or scripting runtime.

## Motivation

Complete prompts need shared communication, delivery, and formatting guidance,
alongside specialized instructions. Copying that guidance into every role makes
consistent maintenance difficult. Template variables, boolean sections, and
rooted partials provide the needed composition without an expression language.

Reference implementations reviewed during design:

- [Gemini CLI](https://geminicli.com/docs/cli/system-prompt/) exposes predefined
  substitutions for skills, subagents, and tool names.
- [Aider](https://github.com/Aider-AI/aider/blob/main/aider/coders/base_coder.py)
  uses string formatting with variables such as language and platform, with
  conditional selection in application code.
- [Goose](https://github.com/block/goose/blob/main/crates/goose/src/prompt_template.rs)
  uses MiniJinja; its system template includes variables, conditions, and loops.
- [smolagents](https://github.com/huggingface/smolagents/blob/main/src/smolagents/agents.py)
  renders Jinja templates with strict undefined-variable handling.

Those projects also render runtime capabilities. Morph already owns tools,
skills, environment, and permissions in separate runtime layers; this
renderer must not duplicate those layers.

## API and data model

`morph_prompt_template_render(text, variables, count, out, error)` renders a
NUL-terminated in-memory string. It does no file I/O; partials are rejected with
a diagnostic, since there is no implicit file resolver or current-directory root.

`morph_prompt_template_render_file(root, entry, variables, count, out, error)`
reads an entry file below an explicit bundle root and supports rooted partials.
Both expand the template into caller-owned text.
The caller frees successful output with `free()`. On any failure, `*out` is NULL;
no partial result is returned and no file is created or modified.

Variables are a flat array of named, typed bindings: UTF-8 strings or booleans.
Dotted names are flat keys, not object traversal. Names consist of ASCII
identifier segments separated by dots; each segment starts with a letter or
underscore and continues with letters, digits, or underscores. Duplicate names,
invalid types, NULL strings, and malformed UTF-8 values are rejected. Empty
strings and false booleans are valid explicit values. Bindings are borrowed for
the call, and the renderer has no global state.

Error details contain a bundle-relative source path (or `<string>`), a 1-based
line and byte column, and an English explanation. The return value is a negative errno for
I/O, allocation, or resource limits, or `MORPH_ERR_FORMAT` for template errors.
Callers display `morph_strerror(rc)` with the optional diagnostic; they must not
log variable values, which may contain user content.

For example, a prompt producer can render its own capability snapshot:

```c
#include "prompt/template.h"
#include <stdlib.h>

struct morph_prompt_template_variable variables[] = {
	{"assistant.name", MORPH_PROMPT_TEMPLATE_STRING, "Morph", 0},
	{"capabilities.vision", MORPH_PROMPT_TEMPLATE_BOOL, NULL, 1},
};
char *prompt = NULL;
struct morph_prompt_template_error error = {0};
int rc = morph_prompt_template_render(
	"You are {{assistant.name}}.\n"
	"{{#capabilities.vision}}Image understanding is configured.\n"
	"{{/capabilities.vision}}",
	variables, sizeof(variables) / sizeof(variables[0]), &prompt, &error);
/* On success, the caller consumes prompt before freeing it. */
free(prompt);
```

The boolean in this example is supplied by the caller. It is not evidence that
a tool is currently enabled or a model endpoint has been tested successfully.

## Syntax version 1

| Syntax | Meaning |
| --- | --- |
| `{{scenario.role}}` | Insert a string binding verbatim |
| `{{#style.concise}}...{{/style.concise}}` | Render a block when a boolean is true |
| `{{^style.concise}}...{{/style.concise}}` | Render a block when a boolean is false |
| `{{>common/delivery.txt}}` | Include a file relative to the bundle root |
| `{{! explanation}}` | Discard a comment |
| `\{{name}}` | Emit literal `{{name}}` |
| `\\` | Emit one literal backslash |

ASCII spaces, tabs, CR, and LF around tag contents are ignored. Section names
must match their closing tags exactly. Sections may nest and span lines.
Partials inherit the same immutable bindings, and each file must balance its
own sections. Partial paths are exact filenames; there is no implicit extension
or resolution relative to the including file.

Whitespace outside tags is preserved exactly, including final newlines. There
is no standalone-tag line removal, indentation propagation, automatic trimming,
or HTML escaping. Authors place tags inline if they do not want blank lines.
Strings are inserted once and never recursively interpreted as template text.
Unrecognized tag forms, including triple braces, expressions, and delimiter
changes, are errors. A closing `}}` outside a tag is ordinary text.

All branches are validated, including false branches: their variable references,
partial references, and syntax must be valid. Consequently a missing partial or
unknown variable cannot remain hidden until a preset option changes. Boolean
sections accept only booleans; substitutions accept only strings. There is no
truthiness conversion, implicit default, or silent missing-variable fallback.

No loops, filters, comparisons, functions, arbitrary environment-variable
lookups, command execution, or template-defined bindings are supported.

## Filesystem and resource limits

The caller selects a bundle root, which is opened once as a directory. Entry
and partial paths are nonempty relative paths with ASCII letters, digits,
underscores, hyphens, dots, and slash separators. Empty components, `.` and `..`
components, absolute paths, backslashes, and symlinks below the opened root are
rejected. Only regular files are read; special files cannot block rendering.
Directory traversal uses `openat` relative to held directory descriptors,
preventing a path-component symlink swap from escaping the root.

Limits are part of the version-1 contract:

- 256 KiB of source per file; 1 MiB of source read per render, counting repeats.
- 1 MiB of rendered output and 1 MiB of aggregate string-binding bytes.
- 16 simultaneously active files, 32 nested sections per file, 128 file loads,
  and 256 variable bindings.
- Entry and partial paths must be shorter than `PATH_MAX` bytes.

Active files are compared by device and inode to reject include cycles even
through hard-link aliases. Repeated nonrecursive includes are permitted. Limits
and cycle checks also apply in false branches. Templates and values must contain
valid UTF-8; files with embedded NUL are rejected. Use the project UTF-8 helpers.

## Rendering lifecycle and integration contracts

The renderer has no predefined variables. A caller can supply `role.name`,
`language`, or `capabilities.vision` if those names are part of that caller's
documented context. Values are snapshots; the module does not inspect config,
environment variables, memory, model endpoints, or permission state. Conditions
are presentation decisions, not enforcement of capability or permission rules.

For request-time templates, the caller constructs fresh bindings at the right
boundary and chooses where stable template content and dynamic context appear
in the model request. The module does not cache mutable values or schedule
refreshes. Do not move runtime facts into a stable prompt prefix unnecessarily.
Never render arbitrary user messages, retrieved text, tool outputs, or previously
rendered content as templates. Existing `[prompt]` files remain literal text;
automatic runtime template loading needs an explicit opt-in design separately.

Setup is one consumer. Future preset generation renders a prompt snapshot once:

1. Collect a preset selection and model choices without writing files.
2. Build an explicit variable context from preset metadata and setup settings.
3. Render and validate the complete prompt in memory.
4. Generate and schema-validate TOML referencing its final prompt snapshot.
5. Publish the prompt and then configuration with no-overwrite behavior; handle
   cleanup and crash leftovers as part of the setup save operation.

The general template renderer implements step 3 only. It does not publish files,
select append/replace mode, change configuration, or decide whether setup may finish.
Template bundle resources can use the existing `prompts/` staging/install rules;
the role phase will resolve that installed bundle and select its entry file.

Do not freeze the current date, cwd, tool list, activated skills, current model
availability, or granted permissions into setup snapshots. Those facts may
change after setup. Enabling dynamic templates in persisted user prompts would
require a separate opt-in format, runtime context contract, and integration with
prompt loading; existing plain-text files must retain literal semantics.

## Role presets need a separate configuration contract

A role will likely comprise more than a template. Before adding roles, design a
structured preset manifest covering these concerns:

| Concern | Required distinction |
| --- | --- |
| Models | Required versus recommended text, vision, image, or video capability; never infer vision from image generation |
| Permissions | Suggested narrowly scoped profiles versus explicit user grants; selecting a role cannot itself authorize access |
| Key files | Shipped assets, generated starter files, and user-supplied inputs; required/optional, destination, collision and ownership rules |
| Guidance files | Whether a file is instructions or reference material; preserve existing project guidance scopes and precedence |
| Tools and skills | Recommended integrations versus capabilities actually registered and available at runtime |
| Validation | Blocking unmet prerequisites versus optional limitations; define whether saving an incomplete draft is allowed |
| Lifecycle | Existing config, customized snapshots, preset versions, upgrades, switching roles, and rollback |

For example, a video role that writes scripts need not require a video model;
a preset promising actual video generation may require one. Photo critique may
require vision, whereas image generation needs an image model. Capability
requirements belong in executable preset metadata and setup validation, not in
natural-language prompt conditions. Configuring a model is also distinct from
having valid credentials and a functioning endpoint.

This phase adds no manifest schema or role policies. A future manifest can name
the template entry and supply its static bindings without changing syntax v1.

## Verification

Unit tests exercise exact text output, UTF-8, strict typing and names, nested and
inverted sections, literal escaping, rooted partial composition, diagnostics,
all-branch validation, cycles, symlinks, special files, and resource limits.
They also load a rendered snapshot through the existing prompt loader to prove
that no runtime template support is required. Existing setup tests verify that
the current onboarding flow remains unchanged. No real model calls are needed.
