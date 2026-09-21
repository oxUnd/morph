# Local dependency patches

The checked-in vendor sources already include these patches. Do not apply them
again during normal builds. Keep dependency fixes separate from project changes.

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
