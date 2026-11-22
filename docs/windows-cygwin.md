# Windows builds with Cygwin

Release packages target Windows 10 version 2004 (build 19041) or newer and
Windows 11, x64. Windows 11 has been validated on a real machine. Use Windows
Terminal 1.22 or newer for Sixel image previews. The ZIP includes the Cygwin
runtime and does not require a separate Cygwin installation.

Morph can target Windows x86-64 through Cygwin's POSIX runtime. The executable
depends on `cygwin1.dll`; it does not require Wine on Windows. Keep all linked
libraries in the same Cygwin runtime family rather than mixing in MinGW/MSYS2
libraries.

## Build in Cygwin

Install these packages with the [Cygwin installer](https://cygwin.com/install.html):

```text
gcc-core gcc-g++ cmake ninja git pkg-config meson make
libcurl-devel libsqlite3-devel libuv-devel libreadline-devel
libfreetype-devel libharfbuzz-devel libcairo-devel libpango1.0-devel
libwebp-devel libffi-devel libxml2-devel libexpat-devel
libjpeg-devel libpng-devel
```

Build libvips separately; it is not in the Cygwin package repository used for
this build. The following configuration includes the installed image codecs
and builds the C API needed by the JavaScript runner:

```sh
curl -fLO https://github.com/libvips/libvips/releases/download/v8.18.7/vips-8.18.7.tar.xz
echo '5baaead3b0bb20ffdb9e9ff09aa9fda08620923df77b63b436654cb5e0b3bf94  vips-8.18.7.tar.xz' | sha256sum -c -
tar -xf vips-8.18.7.tar.xz
meson setup vips-build vips-8.18.7 --prefix=/usr/local --buildtype=release \
    -Dintrospection=disabled -Dexamples=false -Dcplusplus=false -Dmodules=disabled
meson compile -C vips-build
meson install -C vips-build
export PATH="/usr/local/bin:$PATH"
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
```

Prepare the MathJax and Markdown source checkouts from the [build instructions](../README.md#build). From the Morph repository, configure and build:

```sh
cmake -S . -B out/cygwin -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
cmake --build out/cygwin
out/cygwin/bin/morph.exe --help
out/cygwin/bin/morph.exe --version
```

The build stages runtime data under `out/cygwin/share/morph`. Keep this tree
beside `bin` when moving the executables. The CLI reserves a 32 MiB Windows
stack because its path-containing configuration objects exceed Cygwin's default
2 MiB stack. QuickJS needs Cygwin's `malloc.h` declaration, and wasm3 needs a
strong definition of its default yield hook for PE/COFF linking.

## Bundle the runtime

`scripts/package-cygwin.py` stages both executables, runtime data, common shell
commands, the recursive DLL dependency closure, fonts, and a CA certificate
bundle. It rejects unknown DLL dependencies instead of silently omitting them.
The output directory must not exist before packaging.

```sh
python3 scripts/package-cygwin.py \
    --build-dir out/cygwin \
    --sysroot / \
    --objdump /usr/bin/objdump.exe \
    --output out/morph-windows-cygwin \
    --ca-bundle /etc/pki/tls/certs/ca-bundle.crt \
    --archive
```

On Windows, run the generated `morph.cmd` from Windows Terminal or another
console. Keep `bin`, `etc`, `usr`, `home`, and `tmp` together. The bundle puts
resources in `usr/share/morph` because Cygwin exposes the root `bin` directory
as `/usr/bin`. Model
credentials are configured separately through `morph.cmd setup`; packaging
copies only `config.toml.example`, not a user's configuration or credentials.
Additional developer tools such as Git, Node.js, and Python are separate.
Use the generated ZIP when transferring the bundle: it preserves the empty
`tmp` and `home` directories. The `tmp` directory is required during runtime
initialization even before any shell command is executed. The adjacent
`.zip.sha256` file records the archive checksum.

The pinned TUI uses environment-based capability detection on native Windows
console streams. It avoids probe replies appearing in the composer while
retaining active capability queries on transparent Cygwin PTYs such as mintty.

Windows Terminal 1.22 or newer is detected through `WT_SESSION` and uses Sixel
image previews and truecolor without startup queries. Sixel preserves pixel
detail with a 216-color palette. The file sidebar uses colored half-block
characters when the console has no native graphics protocol; `--no-color`
keeps the textual image reference.
The CLI uses cell differences when repainting, so scrolling does not resend
unchanged text and backgrounds across whole rows.
Windows ScrollView wheel input moves three content rows or columns per
terminal report. Arrow keys still move one row, and scrollbar dragging and
explicit content offsets keep their existing coordinates. Wheel reports are
processed in order, including fine wheel increments accumulated by the terminal.

The launcher uses the Windows user profile as `HOME` when it is not already
configured, keeping `.morph` settings outside the program directory. It also
selects the bundled CA certificates unless a certificate environment override
is already set. The bundle includes `cygpath` and the terminal capability data
needed by Readline.

Pass `--icon path/to/morph.ico` when packaging to include a Windows icon. After
extracting the bundle, create a desktop shortcut from PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install-desktop-shortcut.ps1
```

The installer uses the user's actual desktop folder, including OneDrive
redirection, and opens Morph in the user's `Work` directory when it exists.
It starts a new Windows Terminal window when `wt.exe` is available, falling
back to the regular command console otherwise. Existing running windows keep
their current host; reopen the desktop shortcut to use the selected terminal.

## Docker and Wine validation

[MSYS2's Docker project](https://github.com/msys2/msys2-docker) provides the
experimental `ghcr.io/msys2/cygwin-docker-experimental` image, with Wine patches
for Cygwin. Its MSYS2 image is a different runtime and must not supply Morph's
target libraries.

The November 2026 build was cross-compiled on macOS ARM64 with Clang 21 and
GNU binutils 2.45.1 against Cygwin 3.6.12, GCC runtime 14.4.0, and libvips
8.18.7. The generated toolchain, package snapshot, logs, and bundle are under
`out/windows-cygwin/`. This route avoided running Cygwin's compiler subprocesses
inside Wine. Cygwin's `stdatomic.h` in that SDK has an `___wchar_t` spelling
incompatible with Clang; the local SDK copy uses `__wchar_t` instead.

Wine tests cover executable discovery, CLI help/version, JavaScript JSON-RPC,
and a WebAssembly addition through the runner. These are smoke tests, not a
complete Windows test suite. Docker Desktop's Rosetta translation can fail in
Wine/Cygwin subprocesses with an xstate assertion or a fork error. Such a
failure does not establish behavior on a real Windows machine.

## Platform limits

Cygwin does not implement Linux seccomp/Landlock or macOS Seatbelt. Morph keeps
its existing fail-closed behavior when an operation requests unsupported OS
isolation. Explicit host execution remains subject to the existing shared
permission policy. The Cygwin build does not turn a sandboxed operation into
host execution automatically.

The bundled libvips build includes the codecs available in its SDK; additional
formats such as TIFF and HEIF require their libraries when rebuilding libvips.
Interactive terminal behavior and OS integration still need validation on
Windows.
