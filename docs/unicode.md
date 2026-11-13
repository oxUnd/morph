# Unicode support

Project code includes `nb/utf8.h` and links the pinned `nbase::nbase` dependency.
The morph Unicode wrapper and its compatibility names have been removed.
Generic UTF-8 handling and Unicode properties belong to nbase; terminal protocol
handling and token estimation retain their application owners.

The nbase snapshot vendors utf8proc 2.12.0 (Unicode 18.0.0) at
`vendor/nothing/vendor/utf8proc/`, pinned to upstream commit
`79cdd5ab40c79afa559f689ffc13b76812dee1ac`. Its generated database and
MIT/Unicode data license are included. The backend is compiled once into nbase;
no system Unicode package or build-time download is required.

`vendor/nothing/include/nb/utf8.h` defines the public contracts:

- `nb_utf8_decode` consumes a bounded `nb_str` through a caller-owned byte index.
  `nb_utf8_read` decodes a single bounded code point and reports its byte length.
  Invalid, overlong, truncated, surrogate and out-of-range sequences fail.
- `nb_utf8_validate` returns `NB_UTF8_OK` or a diagnostic category, with an optional
  first-error byte offset. Length-aware validation includes embedded NUL bytes;
  applications that forbid NUL explicitly check their input format.
- `nb_utf8_count` counts a valid slice and returns `SIZE_MAX` for malformed input.
  The C-string traversal `nb_utf8_next_cstr` advances one byte with U+FFFD when
  decoding fails, allowing display traversal to make progress.
- `nb_utf8_encode` writes one to four bytes and returns zero for an invalid scalar
  or null destination. It does not append a terminator.
- `nb_utf8_width_cp` uses utf8proc's Unicode width data, independently of locale.
  Combining and nonprinting characters have zero width; ambiguous characters
  use narrow width. Invalid scalars return -1. This measures code points, not
  complete grapheme/emoji rendering by a particular terminal.
- Sanitizing drops invalid bytes and embedded NUL while preserving subsequent
  valid text. Byte clipping never splits a valid code point.
- `nb_utf8_classify_codepoint` groups Unicode categories for case, letters,
  marks, numbers and separators. The emoji-component property operation supplies
  data for conservative token weights; those weights are tokenizer policy.
- `nb_utf8_casefold` appends NFC-normalized, fully case-folded text to an owned
  `nb_buf`. It preserves existing output on invalid input. Database text matching
  owns its buffers and borrows their C-string views only during comparison.

`<morph/tui/terminal/text.h>` owns ANSI CSI/OSC/string removal, streamed terminal
sanitization, CR/LF and visible-control handling, display-column budgets and
ellipsis placement. It uses the same nbase Unicode primitives. Its streaming
sanitizer keeps incomplete UTF-8 between chunks and clears partial protocol
state when a field ends. These are terminal policies, not general Unicode APIs.

`agent/tokenizer.c` owns CJK/language-block token weighting. `agent/bpe.c` uses
native category/decoding APIs for BPE segmentation. Markdown fullwidth punctuation
recognition is a parser rule expressed in the parser, rather than a second
Unicode decoder. Source text and filenames are not normalized implicitly.

The regression suites cover strict scalar boundaries, malformed and truncated
inputs, Unicode 18 width additions, category classification, canonical casefold,
terminal sequence stripping and every streamed chunk boundary.
