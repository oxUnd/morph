# Local dependency patches

The checked-in stb and cJSON sources already include their patches; do not apply
those again during normal builds. MathJax is an ignored, separate source checkout:
its tracked patch is applied during CMake configure after a forward/reverse check.
Keep dependency fixes separate from project changes.

## stb_image_write: unsigned JPEG bit buffer

- Patch: `stb_image_write-unsigned-jpeg-bit-buffer.patch`
- Base: bundled `stb_image_write.h` v1.16, SHA-256 before patch:
  `cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05`
- Upstream: <https://github.com/nothings/stb/blob/master/stb_image_write.h>
- Checked on 2026-09-21: upstream master still identifies as v1.16 and uses
  a signed JPEG bit buffer; the same shift remains present.
- Reason: UBSan reports an unrepresentable signed left shift while emitting
  JPEG bytes. Use unsigned arithmetic for the bit buffer and input shift,
  preserving the intended bit pattern. This is a correctness fix discovered
  during memory testing, not a memory optimization. Public APIs are unchanged.

After replacing the header with an upstream version, run from the repository root:

```sh
git apply --check vendor/patches/stb_image_write-unsigned-jpeg-bit-buffer.patch
git apply vendor/patches/stb_image_write-unsigned-jpeg-bit-buffer.patch
```

To check whether the patch is already applied:

```sh
git apply --reverse --check vendor/patches/stb_image_write-unsigned-jpeg-bit-buffer.patch
```

If the forward check fails, inspect upstream before adapting the patch. If upstream
has fixed the issue, remove the obsolete patch and update this document. Otherwise,
regenerate the patch against the newly imported pristine header, update the base
version/hash, and verify reverse/forward application restores identical bytes.

Validate an update with the normal test suite and the sanitizer build:

```sh
cmake -S . -B build-asan -DENABLE_ASAN=ON
cmake --build build-asan
UBSAN_OPTIONS=halt_on_error=1 ./build-asan/bin/morph-tests
```

## stb_image_resize2: explicit vertical dispatch

- Patch: `stb_image_resize2-explicit-dispatch.patch`
- Base: bundled `stb_image_resize2.h` v2.18, SHA-256 before patch:
  `c29b2480e569e1069b338a4d1516971bbe734f302aabe26df98514a0178313fe`
- Upstream: <https://github.com/nothings/stb/blob/master/stb_image_resize2.h>
- Reason: GCC 12's optimized object-size instrumentation reports insufficient
  space when loading a function pointer through a conditional expression that
  selects one of two eight-element arrays. The contributing scanline count is
  bounded to 1–8. Select the already indexed element instead, retaining the
  declared array extent at each access. Both gather and scatter dispatch use
  the same arithmetic and functions as before. No sanitizer is disabled.
- Regression: `ImageGen.ProbeAndResizeFileExact` reproduces the diagnostic on
  Linux arm64 with ASan/UBSan. The additional
  `ImageGen.ResizeFiltersPreserveConstantChannelsAcrossVerticalRatios` checks
  240 combinations of channel counts, up/downsampling ratios and filters.

The header already includes this patch. Reverse-check it with
`git apply --reverse --check vendor/patches/stb_image_resize2-explicit-dispatch.patch`;
after importing a pristine replacement, forward-check before applying it. Follow
the same provenance and byte-identical round-trip rules as the JPEG patch above.

```sh
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ./build-asan/bin/morph-tests \
  --gtest_filter='ImageGen.*:ImageEncode.*:ImgGenToolTest.*'
```

## cJSON: explicit owners and rejection of linked items

`cJSON_DeleteOwner`, `cJSON_FreeTextOwner`, and `cJSON_StealOwner` operate on
caller-owned pointers for lexical cleanup and transfer. They use cJSON's own
allocator hooks. Applications now call the native serialization and insertion
APIs directly and clear their source owner only after insertion succeeds.

Native AddItemToArray/AddItemToObject reject nodes whose sibling links show
that they already belong to a container, preventing duplicate insertion from
creating corrupt lists or double ownership. Allocation failure leaves the
source owner available to the caller. The checked-in patch records these
changes: `cjson-ownership.patch`.

Regressions: `Cleanup.JsonStringCopyAndOwnerTransfer`,
`Cleanup.JsonStringCopyRetainsSourceOnAllocationFailure`, and
`Cleanup.NativeJsonContainersPreserveOwnersUntilAccepted`.

## MathJax: parser configuration ownership

- Patch: `mathjax-parser-ownership.patch`
- Base checkout: `ea692adccc0eb56ac53261c5880d93094d22e43e`
- Scope: `mjx_parse_opts.owns_options` defaults to borrowed configuration;
  successful owned creation clears the source and parser destruction releases
  its factory, macros, operator array and environment names/arrays. Registrations
  against borrowed arrays allocate parser-owned copies without freeing the caller's
  storage. Multi-array environment registration commits only after all allocations
  succeed. Default context construction cleans up partial initialization failures.
- Reproduction: Linux LeakSanitizer reported 73,362 bytes in 318 allocations from
  `test_md_html`; operators, default macros/factory and environments survived
  parser destruction.
- Configure hook: `cmake/ApplyMathJaxPatch.cmake` checks reverse application first,
  then checks and applies the forward patch if needed. It rejects mismatched local
  source instead of replacing it. The already patched read-only source path used
  in Docker passes the reverse check without writing files. The source path is
  canonicalized, inherited Git repository variables are cleared, and discovery
  stops at its parent. This prevents an archive without its own `.git` from
  inheriting Morph's prefix and silently skipping every patch hunk. Four fixture
  layouts (parent Git, child Git, no Git, and a symlink to another Git parent)
  each pass fresh application, idempotence, unrelated-change preservation and
  conflicting-change rejection checks.
- Regression target: `morph-mathjax-ownership-tests`, CTest
  `mathjax_parser_ownership`; tests owned transfer, borrowed stack resources with
  local registrations, failure preservation and repeated context/parse teardown.
  The original `test_md_html` must also pass with LeakSanitizer enabled.
