#!/usr/bin/env python3
"""Create native extensions using only resources shipped inside this skill."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tempfile


SKILL = Path(__file__).resolve().parents[1]
ASSETS = SKILL / "assets"
VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")


def available_versions():
    return sorted(
        (p.name for p in (ASSETS / "sdk").iterdir()
         if p.is_dir() and VERSION.fullmatch(p.name)),
        key=lambda value: tuple(int(part) for part in value.split(".")),
    )


def verify_snapshot(directory, expected_version=None):
    metadata = json.loads((directory / "manifest.json").read_text())
    if not isinstance(metadata, dict):
        raise ValueError("Invalid SDK manifest")
    version = metadata.get("sdk_version", "")
    if (metadata.get("format_version") != 1 or
            metadata.get("name") != "morph-ext-sdk" or
            not isinstance(version, str) or not VERSION.fullmatch(version) or
            not isinstance(metadata.get("abi_version"), int) or
            metadata["abi_version"] < 1):
        raise ValueError("Invalid SDK manifest")
    if expected_version is not None and version != expected_version:
        raise ValueError("SDK directory and manifest versions differ")

    files = metadata.get("files")
    if not isinstance(files, dict) or not files:
        raise ValueError("SDK manifest has no file hashes")
    for name, digest in files.items():
        relative = PurePosixPath(name)
        if (relative.is_absolute() or ".." in relative.parts or
                not relative.parts or relative.parts[0] != "include" or
                not isinstance(digest, str) or not re.fullmatch(r"[a-f0-9]{64}", digest)):
            raise ValueError("Invalid SDK file record")
        path = directory / relative
        path.resolve().relative_to(directory.resolve())
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"SDK checksum mismatch: {name}")

    actual = {p.relative_to(directory).as_posix()
              for p in (directory / "include").rglob("*") if p.is_file()}
    if actual != set(files):
        raise ValueError("SDK file list differs from the pinned manifest")
    header = (directory / "include/morph/ext.h").read_text()
    declared_version = re.search(r'^#define EXT_SDK_VERSION "([^"]+)"$', header, re.M)
    declared_abi = re.search(r"^#define EXT_CONTEXT_ABI_VERSION ([0-9]+)u?$", header, re.M)
    if (not declared_version or declared_version[1] != version or
            not declared_abi or int(declared_abi[1]) != metadata["abi_version"]):
        raise ValueError("SDK header and manifest versions differ")
    return metadata


def load_bundle(version):
    versions = available_versions()
    if version not in versions:
        raise ValueError(f"SDK {version} is not bundled; available: {', '.join(versions)}")
    bundle = ASSETS / "sdk" / version
    return bundle, verify_snapshot(bundle, version)


def copy_sdk(bundle, destination, metadata):
    destination.mkdir()
    for name in metadata["files"]:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(bundle / name, target)
    shutil.copyfile(bundle / "manifest.json", destination / "manifest.json")


def initialize(destination, name, version):
    if not re.fullmatch(r"[a-z][a-z0-9_-]{0,62}", name):
        raise ValueError("Name must start with a lowercase letter and contain 1-63 lowercase letters, digits, underscores or hyphens")
    bundle, metadata = load_bundle(version)
    templates = sorted((ASSETS / "native").glob("*.tmpl"))
    required = {"plugin.c.tmpl", "Makefile.tmpl", "manifest.toml.tmpl", "test_plugin.c.tmpl"}
    if not required.issubset({p.name for p in templates}):
        raise ValueError("Skill package is missing native scaffold templates")
    if destination.exists() and (not destination.is_dir() or any(destination.iterdir())):
        raise ValueError("init requires a new or empty plugin directory")

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".morph-ext-init-", dir=destination.parent) as tmp:
        staged = Path(tmp) / "plugin"
        staged.mkdir()
        replacements = {"@NAME@": name, "@SDK_VERSION@": version,
                        "@ABI_VERSION@": str(metadata["abi_version"])}
        for template in templates:
            text = template.read_text()
            for key, value in replacements.items():
                text = text.replace(key, value)
            filename = template.name.removesuffix(".tmpl")
            if filename == "gitignore":
                filename = ".gitignore"
            (staged / filename).write_text(text)
        copy_sdk(bundle, staged / "sdk", metadata)
        if destination.exists():
            destination.rmdir()
        os.rename(staged, destination)
    print(f"Created {name}: {destination} (SDK {version}, ABI {metadata['abi_version']})")
    print(f"Build and test: make -C {destination} test all")


def export_sdk(destination, version, replace):
    bundle, metadata = load_bundle(version)
    sdk = destination / "sdk"
    if sdk.is_symlink():
        raise ValueError("Refusing to replace a symbolic-link SDK directory")
    if sdk.exists():
        try:
            current = verify_snapshot(sdk)
        except (OSError, ValueError):
            current = None
        if current == metadata:
            print(f"SDK {version} already pinned and verified: {sdk}")
            return
        if not replace:
            raise ValueError("SDK already exists; use --replace to explicitly update only sdk/")

    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".morph-ext-sdk-", dir=destination) as tmp:
        staged = Path(tmp) / "sdk"
        backup = Path(tmp) / "previous-sdk"
        copy_sdk(bundle, staged, metadata)
        if sdk.exists():
            os.rename(sdk, backup)
        try:
            os.rename(staged, sdk)
        except OSError:
            if backup.exists():
                os.rename(backup, sdk)
            raise
    print(f"Pinned SDK {version}, ABI {metadata['abi_version']}: {sdk}")


def check_sdk(destination):
    metadata = verify_snapshot(destination / "sdk")
    _, bundled = load_bundle(metadata["sdk_version"])
    if metadata != bundled:
        raise ValueError("Pinned SDK manifest differs from this version's bundled release")
    print(f"Verified SDK {metadata['sdk_version']}, ABI {metadata['abi_version']}: {destination / 'sdk'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    init = commands.add_parser("init", help="Generate a buildable native plugin and pinned SDK")
    init.add_argument("directory", type=Path)
    init.add_argument("--name", help="Tool name; defaults to the destination directory name")
    init.add_argument("--sdk-version", help="Exact bundled SDK version; default: newest bundled version")
    sdk = commands.add_parser("sdk", help="Generate sdk/ in an existing plugin without touching its code")
    sdk.add_argument("directory", type=Path)
    sdk.add_argument("--sdk-version", required=True, help="Exact bundled SDK version")
    sdk.add_argument("--replace", action="store_true", help="Explicitly replace an existing sdk/ snapshot")
    check = commands.add_parser("check", help="Verify a plugin's SDK version, ABI and file hashes")
    check.add_argument("directory", type=Path)
    commands.add_parser("versions", help="List SDK releases included in this skill package")
    args = parser.parse_args()
    try:
        if args.command == "versions":
            for version in available_versions():
                _, metadata = load_bundle(version)
                print(f"{version} (ABI {metadata['abi_version']})")
        elif args.command == "init":
            versions = available_versions()
            if not versions:
                raise ValueError("No SDK releases are bundled with this skill")
            initialize(args.directory.absolute(), args.name or args.directory.absolute().name,
                       args.sdk_version or versions[-1])
        elif args.command == "sdk":
            export_sdk(args.directory.absolute(), args.sdk_version, args.replace)
        else:
            check_sdk(args.directory.absolute())
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"scaffold: {error}\n")


if __name__ == "__main__":
    main()
