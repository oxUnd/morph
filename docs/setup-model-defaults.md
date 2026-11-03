# Setup model defaults

Verified against the official provider documentation on 2026-11-03. These are
Morph's deployment recommendations for new configurations. They balance room for
tool use, reasoning and answers with response latency. Output limits are ceilings,
not a request to generate that many tokens.

| Provider / model | Context tokens | Default output tokens | API output ceiling | Thinking |
| --- | ---: | ---: | ---: | --- |
| OpenAI `gpt-4o` (text / vision) | 128000 | 16384 | 16384 | Not a reasoning model |
| DeepSeek `deepseek-flash` (text) | 1048576 | 65536 | 393216 | Enabled, `high` |
| DeepSeek `deepseek-flash` (vision) | 1048576 | 16384 | 393216 | Disabled |
| Ark `doubao-seed-2-0-lite-260428` (text / vision) | 262144 | 16384 | 131072 | Disabled |
| Custom / unknown chat models | 128000 | 16384 | User supplied | Provider specific |

The OpenAI figures come from the [GPT-4o model specification](https://developers.openai.com/api/docs/models/gpt-4o).
DeepSeek's [model metadata](https://api-docs.deepseek.com/api/list-models/) gives
the exact binary token limits. Its [Chat Completions reference](https://api-docs.deepseek.com/api/create-chat-completion/)
defines a shared reasoning/answer budget, a 64K default in thinking mode, and a
128K default for maximum reasoning effort. Morph uses 16K for non-thinking replies
to leave more answer headroom than DeepSeek's 8K server default.

`deepseek-v4-flash` and `deepseek-v4-flash-vision-exp` retain the same limits as
`deepseek-flash`: DeepSeek documents these as accepted legacy names for the current
Flash model. `deepseek-v4-pro` has the same token limits. A setup thinking toggle
adjusts a default output budget between 16K and 64K (128K for `max`) while preserving
custom token budgets and unrelated JSON options. Explicit thinking settings take
precedence over `reasoning_effort`.

Ark's [model list](https://docs.volcengine.com/docs/ark/model-list?lang=zh&redirect=1)
lists Seed 2.0 Lite as LTS with a 256K context and 128K maximum answer. K figures
are converted using 1024 tokens per K in the table above. Its
[thinking guide](https://docs.volcengine.com/docs/ark/deep-thinking?lang=zh) requires
replaying encrypted thinking blocks for multi-turn tool calls. The current generic
Chat adapter does not preserve those blocks, so setup explicitly disables thinking
for this preset. The 16K answer budget is a Morph recommendation, not the API limit.

Unknown model names, endpoint IDs and compatible gateways use editable fallback
budgets; setup does not infer capacity from a model prefix or provider alone.
Known model limits still validate user input independently of the recommended
output budget.

## Media and timeouts

- OpenAI image generation keeps `gpt-image-2`, the current [image model](https://developers.openai.com/api/docs/models/gpt-image-2).
- Ark image generation uses `doubao-seedream-5-0-flash-260915`; the previous
  `doubao-seedream-5-0-260128` preset is marked for retirement in Ark's model list.
  Flash supports the existing single-image generation and reference-image paths;
  see the [image generation API](https://docs.volcengine.com/docs/ark/image-generation-api?lang=zh).
- Video generation keeps `doubao-seedance-2-0-mini-260615` as the economical default.
  It supports the reference-image/video/audio workflow; its capabilities and
  alternatives are in the same official model list.

Morph writes its timeout recommendations explicitly: 600 seconds for text,
300 seconds for vision and image generation, and video polling every 5 seconds
for up to 1800 seconds. Chat retains three retries for transient HTTP failures.
These are application defaults, not provider capacity claims. Streaming chat
timeouts measure idle/no-data time rather than total generation duration.

Both terminal and plain setup share these defaults and the same configuration
writer. Reconfiguration preserves existing model IDs, explicit thinking options,
token budgets, retry counts and timeouts unless the user changes them or resets
the provider. Existing configurations are not migrated automatically.
