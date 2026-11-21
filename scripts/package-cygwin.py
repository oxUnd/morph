#!/usr/bin/env python3
"""Stage morph, its resources, and the Cygwin DLL dependency closure."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


SYSTEM_DLLS = {
    "advapi32.dll", "bcrypt.dll", "comctl32.dll", "comdlg32.dll",
    "crypt32.dll", "dnsapi.dll", "gdi32.dll", "imm32.dll", "iphlpapi.dll",
    "kernel32.dll", "msvcrt.dll", "netapi32.dll", "normaliz.dll", "ntdll.dll",
    "ole32.dll", "oleaut32.dll", "psapi.dll", "rpcrt4.dll", "secur32.dll",
    "setupapi.dll", "shell32.dll", "shlwapi.dll", "ucrtbase.dll", "user32.dll",
    "userenv.dll", "usp10.dll", "version.dll", "winmm.dll", "winspool.drv",
    "ws2_32.dll", "wsock32.dll",
}


def copy_tree(source, destination):
    if source.is_dir():
        shutil.copytree(source, destination, ignore_dangling_symlinks=True)


def archive_bundle(output):
    archive = output.with_name(output.name + ".zip")
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=6) as bundle:
        bundle.write(output, output.name)
        for source in sorted(output.rglob("*")):
            bundle.write(source, Path(output.name) / source.relative_to(output))
    with zipfile.ZipFile(archive) as bundle:
        if bundle.testzip():
            raise RuntimeError("archive integrity check failed")
        for name in ("tmp/", "home/"):
            if output.name + "/" + name not in bundle.namelist():
                raise RuntimeError(f"archive is missing runtime directory: {name}")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_name(archive.name + ".sha256").write_text(
        f"{digest}  {archive.name}\n", encoding="utf-8")
    print(f"Archived {archive} (SHA-256 {digest})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--sysroot", type=Path, required=True)
    parser.add_argument("--objdump", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ca-bundle", type=Path, required=True)
    parser.add_argument("--package-manifest", type=Path)
    parser.add_argument("--icon", type=Path)
    parser.add_argument("--archive", action="store_true",
                        help="write a ZIP and SHA-256, preserving empty runtime directories")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output directory already exists")
    if args.archive and args.output.with_name(args.output.name + ".zip").exists():
        parser.error("output archive already exists")
    executables = [args.build_dir / "bin" / name for name in
                   ("morph.exe", "morph-js-runner.exe")]
    required = [*executables, args.ca_bundle, args.objdump,
                args.sysroot / "bin/bash.exe", args.sysroot / "bin/cygpath.exe"]
    if args.icon:
        required.append(args.icon)
    for path in required:
        if not path.is_file():
            parser.error(f"missing input: {path}")
    if b"-----BEGIN CERTIFICATE-----" not in args.ca_bundle.read_bytes():
        parser.error("CA bundle contains no certificates")
    resources = args.build_dir / "share/morph"
    if not resources.is_dir():
        parser.error(f"missing runtime resources: {resources}")

    output = args.output.resolve()
    binary_dir = output / "bin"
    binary_dir.mkdir(parents=True)
    sdk_dirs = [args.sysroot / "bin", args.sysroot / "usr/bin",
                args.sysroot / "usr/local/bin"]
    dlls = {}
    for directory in sdk_dirs:
        for path in directory.glob("*.dll"):
            dlls[path.name.lower()] = path

    pending = list(executables)
    for name in ("bash", "cygpath", "env", "ls", "cat", "pwd", "mkdir", "rm", "cp", "mv",
                 "touch", "chmod", "head", "tail", "wc", "uname", "grep",
                 "find", "sed", "xargs", "readlink", "realpath"):
        path = args.sysroot / "bin" / f"{name}.exe"
        if path.is_file():
            pending.append(path)
    copied = set()
    system_dependencies = set()
    while pending:
        source = pending.pop()
        key = source.name.lower()
        if key in copied:
            continue
        copied.add(key)
        shutil.copy2(source, binary_dir / source.name)
        result = subprocess.run([str(args.objdump), "-p", str(source)],
                                check=True, capture_output=True, text=True)
        for name in re.findall(r"DLL Name:\s*(\S+)", result.stdout):
            dependency = name.lower()
            if dependency in dlls:
                pending.append(dlls[dependency])
            elif dependency in SYSTEM_DLLS or dependency.startswith("api-ms-"):
                system_dependencies.add(name)
            else:
                raise RuntimeError(f"unresolved DLL dependency: {name} ({source})")

    shutil.copy2(binary_dir / "bash.exe", binary_dir / "sh.exe")
    copied.add("sh.exe")
    # Cygwin exposes its root bin directory as /usr/bin as well as /bin.
    copy_tree(resources, output / "usr/share/morph")
    copy_tree(args.sysroot / "usr/share/terminfo", output / "usr/share/terminfo")
    copy_tree(args.sysroot / "usr/share/fonts", output / "usr/share/fonts")
    copy_tree(args.sysroot / "etc/fonts", output / "etc/fonts")
    font_configs = args.sysroot / "usr/share/fontconfig/conf.avail"
    if font_configs.is_dir():
        target = output / "etc/fonts/conf.d"
        target.mkdir(parents=True, exist_ok=True)
        for path in font_configs.glob("*.conf"):
            if not (target / path.name).exists():
                shutil.copy2(path, target / path.name)
    certificates = output / "etc/pki/tls/certs"
    certificates.mkdir(parents=True)
    shutil.copy2(args.ca_bundle, certificates / "ca-bundle.crt")
    (output / "tmp").mkdir()
    (output / "home").mkdir()
    shutil.copy2(Path(__file__).resolve().parents[1] / "config.toml.example",
                 output / "config.toml.example")
    shutil.copy2(Path(__file__).with_name("install-cygwin-shortcut.ps1"),
                 output / "install-desktop-shortcut.ps1")
    if args.icon:
        shutil.copy2(args.icon, output / "morph.ico")
    (output / "morph.cmd").write_bytes(
        b'@echo off\r\nsetlocal\r\nset "PATH=%~dp0bin;%PATH%"\r\n'
        b'if not defined HOME for /f "delims=" %%H in (\'""%~dp0bin\\cygpath.exe" -u "%USERPROFILE%""\') do set "HOME=%%H"\r\n'
        b'if not defined TERM set "TERM=xterm-256color"\r\n'
        b'if not defined CURL_CA_BUNDLE set "CURL_CA_BUNDLE=%~dp0etc\\pki\\tls\\certs\\ca-bundle.crt"\r\n'
        b'if not defined SSL_CERT_FILE set "SSL_CERT_FILE=%~dp0etc\\pki\\tls\\certs\\ca-bundle.crt"\r\n'
        b'"%~dp0bin\\morph.exe" %*\r\nexit /b %ERRORLEVEL%\r\n')
    (output / "README.txt").write_text(
        "Morph for Windows x86-64 (Cygwin)\n\n"
        "Run morph.cmd from Windows Terminal, or bin/morph.exe from a Cygwin shell.\n"
        "The Cygwin DLLs and common shell commands are included. Keep bin, etc,\n"
        "usr, tmp, and home together. Configure models with morph.cmd setup.\n"
        "The launcher defaults HOME to your Windows user profile, so settings\n"
        "remain available when you replace or move the program directory.\n"
        "Run install-desktop-shortcut.ps1 in PowerShell to create a desktop\n"
        "shortcut with the bundled icon. It uses the actual Windows desktop,\n"
        "including desktops redirected to OneDrive.\n"
        "Additional developer tools such as Git, Node.js, and Python are separate.\n\n"
        "Cygwin does not provide Morph's Linux/macOS OS sandbox. Sandboxed exec\n"
        "continues to reject unsupported isolation; explicit host execution is\n"
        "subject to the existing permission policy.\n",
        encoding="utf-8")
    manifest = {"files": sorted(copied),
                "windows_system_dependencies": sorted(system_dependencies)}
    (output / "runtime-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    if args.package_manifest:
        shutil.copy2(args.package_manifest, output / "cygwin-packages.json")
    print(f"Staged {len(copied)} executables and DLLs in {output}")
    if args.archive:
        archive_bundle(output)


if __name__ == "__main__":
    main()
