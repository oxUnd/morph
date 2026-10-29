# Vendored utf8proc

Upstream: https://github.com/JuliaStrings/utf8proc
Release: v2.12.0 (Unicode 18.0.0)
Commit: 79cdd5ab40c79afa559f689ffc13b76812dee1ac

The upstream `utf8proc.c`, `utf8proc.h`, `utf8proc_data.c`, and `LICENSE.md` are
vendored unchanged. The implementation includes the generated Unicode database;
normal builds neither download nor regenerate Unicode data.

The shared `src/util/sources.list` manifest includes `utf8proc.c` for desktop and
mobile hosts. CMake compiles it into the static `morph-util` target.
Project code includes `util/utf8.h`, which provides utf8proc-backed compatibility
helpers and the application's terminal sanitizing, clipping, and layout policies.
See `LICENSE.md` for the MIT and Unicode data licenses.
