# Image understanding and upload fidelity

`img_qa` sends one local image and its question to the independent `[model.vision]`
model. The main agent receives the vision model's text answer, rather than the
image itself. The vision request contains the tool's system instruction and
`prompt`; it does not include the main conversation. Include the context needed
to answer the question in `prompt`.

## Local preparation and API detail

These are separate controls: the local size cap determines the pixels uploaded,
while the API's `detail` hint controls provider preprocessing. Preserving upload
dimensions does not guarantee that the model processes every original pixel.

| `img_qa` detail | Default local maximum side | API `image_url.detail` |
| --- | --- | --- |
| Omitted or `auto` | Original dimensions within shared safety limits | `auto` |
| `high` | 2048 pixels | `high` |
| `original` | Original dimensions within shared safety limits | `original` |

`max_dim` overrides the local maximum side independently of API detail. Resizing
preserves aspect ratio and never enlarges small images. `original` requires a
vision model that accepts that API detail value; older models such as GPT-4o do
not. The default `auto` avoids requiring that support.

PNG and JPEG files that fit the local size cap are uploaded byte for byte.
Resized PNG stays lossless PNG; resized JPEG is encoded at quality 88. Other
decodable formats are converted to PNG. Base64 itself does not lose information.
Shared safety limits are 64 MiB per source file, 32 megapixels, and 32768 pixels
per side. Provider limits apply separately and can be lower. For example,
DeepSeek currently accepts at most 8192 pixels per side for a single image; use
`max_dim: 8192` for larger source images.

Before the fidelity fix, omitted `detail` selected a local 2048-pixel cap, and
neither `high` nor `original` reached the API. A 1200 x 6000 screenshot therefore
arrived as 409 x 2048, reducing a 12-pixel text height to about 4 pixels. Changing
JPEG quality cannot recover detail discarded by this resize.

The regression in `tests/test_image.cpp` captures real HTTP request bodies from
a local mock server, decodes the embedded image, and checks its dimensions,
unchanged PNG/JPEG payloads, explicit resizing, and the transmitted detail hint.
It verifies upload fidelity, not a provider's recognition accuracy.

## Lessons from other agents

The following sources were reviewed on 2026-11-23. Their defaults can change.

- [Codex image preparation](https://github.com/openai/codex/blob/main/codex-rs/utils/image/src/lib.rs)
  separates original and resized images and also defines dimension-and-patch
  budgets for high and original detail. Its
  [`view_image` handler](https://github.com/openai/codex/blob/main/codex-rs/core/src/tools/handlers/view_image.rs)
  checks model support for original detail and returns image content to the
  agent's history. This lets the main model reason from the image directly.
- [OpenCode attachment documentation](https://github.com/anomalyco/opencode/blob/dev/services/site/src/docs/content/attachments.mdx)
  describes model-visible image attachments and `read` results. Its default
  normalization limits are 2000 x 2000 pixels and 5 MiB of Base64. Those limits
  are configurable; resizing considers both dimensions and payload bytes.
- [Claude's vision guide](https://platform.claude.com/docs/en/build-with-claude/vision)
  describes model-specific image resolution and patch budgets and warns that
  repeated lossy compression can harm text recognition. A single fixed image
  size is not a universal quality target.

Morph retains separate text and vision models. The immediate improvements are
to preserve source pixels by default and transmit API detail explicitly. Direct
multimodal input to the main model and a dedicated region/tiling interface would
be separate changes.

## Dense images and long screenshots

[DeepSeek's vision documentation](https://api-docs.deepseek.com/guides/vision/)
states that `high`, `original`, and `auto` keep the supplied image at the detail
selection stage. It also describes subsequent inference resizing to roughly
1300 x 1300 pixels of area, with a maximum of 1024 image tokens. Consequently,
`original` cannot be interpreted as a promise of native-resolution inference.

[OpenAI's vision documentation](https://developers.openai.com/api/docs/guides/images-vision)
likewise specifies different size and patch budgets by model and detail level.

For a long screenshot or dense document, inspect the whole image for context,
then make cropped copies of relevant regions and ask focused questions about
those copies. Each region receives its own image budget. Enlarging the whole
image or increasing the answer's `max_tokens` does not recover lost visual
detail. `img_qa` currently accepts one image per call; it does not crop or tile
automatically.
