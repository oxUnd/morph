# Unicode support

The project vendors utf8proc 2.12.0 (Unicode 18.0.0) at
`vendor/utf8proc/`, pinned to upstream commit
`79cdd5ab40c79afa559f689ffc13b76812dee1ac`. Its generated Unicode database and
MIT/Unicode data license are included. The shared `src/util/sources.list` manifest
includes its implementation for desktop and mobile core builds. CMake compiles
it into the static `morph-util` library. No additional system package or network
access is needed to build this dependency.

Project code continues to include `util/utf8.h`. The old sheredom header has been
removed. Existing names such as `utf8valid`, `utf8nvalid`, `utf8len`,
`utf8codepoint`, and `utf8dup` are compatibility adapters, not a second decoder.

- `utf8_decode_codepoint` calls `utf8proc_iterate` with an explicit byte bound.
  Invalid/truncated sequences fail, including overlong encodings, surrogates,
  and code points beyond U+10FFFF. A NUL terminates the project string API;
  length-aware sanitizing drops embedded NUL bytes.
- `utf8_encode_codepoint` checks scalar validity and calls
  `utf8proc_encode_char`. It writes one to four bytes without a terminator,
  and returns zero for invalid code points or a null destination.
- `utf8valid` and `utf8nvalid` return the first invalid byte, or null on success.
  `utf8nvalid` never reads past its byte bound. Compatibility counting treats
  malformed bytes individually; bounded decoding is preferred for new callers.
- `utf8codepoint` advances one byte with U+FFFD on malformed input, allowing
  string traversal to make progress. `utf8codepointcalcsize` uses utf8proc's
  leading-byte table for incomplete stream prefixes; it does not validate input.
  Text traversal uses bounded decoding instead of this prefix-length adapter.
- Character widths come from `utf8proc_charwidth`, independent of process locale.
  Combining and nonprinting characters have zero width; East Asian ambiguous
  characters use the library's default narrow width. These APIs measure code
  points, not complete emoji/grapheme-cluster rendering by a particular terminal.
- Sanitizing drops invalid bytes and preserves valid following text. It does not
  normalize source text or file names. Byte and display clipping use the same
  decoder and never split a valid code point.
- BPE letter/case/mark/number/space classification uses
  `utf8proc_category`, replacing hand-maintained category ranges.
- The fallback token estimator uses utf8proc pictograph, regional-indicator,
  joiner, and extending-character properties for its conservative emoji-component
  weights. These weights are estimation policy, not an emoji-membership API.

ANSI parsing, terminal escape filtering, CR/LF handling, column budgets,
ellipsis placement, and language-block token-weight heuristics remain project
policies. utf8proc does not provide terminal protocol handling or Unicode Script
lookup. Smart-quote parsing and backwards character movement use the common
decoder rather than recognizing encoded byte patterns independently.
