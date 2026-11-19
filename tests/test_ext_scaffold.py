"""Exercise the shipped skill in isolation, without a Morph checkout or build."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SKILL = ROOT / "skills/morph-ext-development"
PUBLIC = ROOT / "include"
CC = "cc"


class ExtScaffoldTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="morph-skill-only-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.skill = self.directory / "detached-skill"
        shutil.copytree(SKILL, self.skill)
        self.plugin = self.directory / "my-plugin"

    def run_script(self, *arguments, success=True):
        result = subprocess.run(
            [sys.executable, str(self.skill / "scripts/scaffold.py"),
             *map(str, arguments)], cwd=self.directory,
            text=True, capture_output=True, timeout=15)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def init(self):
        self.run_script("init", self.plugin, "--name", "my_tool", "--sdk-version", "1.1.0")

    def test_detached_skill_generates_and_builds_without_morph_or_downloads(self):
        self.init()
        self.run_script("check", self.plugin)
        metadata = json.loads((self.plugin / "sdk/manifest.json").read_text())
        self.assertEqual(metadata["sdk_version"], "1.1.0")
        self.assertEqual(metadata["abi_version"], 1)
        environment = dict(os.environ, CFLAGS="-O2 -Werror",
                           MORPH_SRC="/not-installed", MORPH_INC="/not-installed")
        result = subprocess.run(
            ["make", "-C", str(self.plugin), "test", "all", f"CC={CC}"],
            env=environment, text=True, capture_output=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.plugin / "my_tool.so").is_file())
        self.assertIn("ownership checks passed", result.stdout)

    def test_existing_plugin_code_survives_sdk_generation_and_update(self):
        self.plugin.mkdir()
        code = self.plugin / "plugin.c"
        code.write_text("preserve this implementation\n")
        self.run_script("sdk", self.plugin, "--sdk-version", "1.1.0")
        lock = (self.plugin / "sdk/manifest.json").read_bytes()
        self.run_script("sdk", self.plugin, "--sdk-version", "1.1.0")
        self.assertEqual(lock, (self.plugin / "sdk/manifest.json").read_bytes())
        header = self.plugin / "sdk/include/morph/ext.h"
        header.write_text(header.read_text() + "\n/* local edit */\n")
        self.run_script("check", self.plugin, success=False)
        self.run_script("sdk", self.plugin, "--sdk-version", "1.1.0", success=False)
        self.assertIn("local edit", header.read_text())
        self.run_script("sdk", self.plugin, "--sdk-version", "1.1.0", "--replace")
        self.run_script("check", self.plugin)
        self.assertEqual(code.read_text(), "preserve this implementation\n")

    def test_init_preserves_nonempty_existing_projects(self):
        self.plugin.mkdir()
        marker = self.plugin / ".gitignore"
        marker.write_text("preserve\n")
        self.run_script("init", self.plugin, success=False)
        self.assertEqual(marker.read_text(), "preserve\n")
        self.assertEqual(list(self.plugin.iterdir()), [marker])

    def test_unknown_version_and_invalid_name_do_not_create_a_project(self):
        self.run_script("init", self.plugin, "--sdk-version", "99.0.0", success=False)
        self.assertFalse(self.plugin.exists())
        self.run_script("init", self.plugin, "--name", "../escape", success=False)
        self.assertFalse(self.plugin.exists())

    def test_modified_release_is_rejected_before_generating_files(self):
        header = self.skill / "assets/sdk/1.1.0/include/morph/ext.h"
        header.write_text(header.read_text() + "\n/* changed without a version bump */\n")
        result = self.run_script("init", self.plugin, success=False)
        self.assertIn("checksum mismatch", result.stderr)
        self.assertFalse(self.plugin.exists())

    def test_abi_and_sdk_version_metadata_are_verified(self):
        self.init()
        lock = self.plugin / "sdk/manifest.json"
        metadata = json.loads(lock.read_text())
        metadata["abi_version"] = 999
        lock.write_text(json.dumps(metadata))
        self.run_script("check", self.plugin, success=False)

    def test_new_sdk_release_requires_an_explicit_update_and_keeps_old_release(self):
        self.init()
        old = self.skill / "assets/sdk/1.1.0"
        newer = self.skill / "assets/sdk/1.1.1"
        shutil.rmtree(newer, ignore_errors=True)
        shutil.copytree(old, newer)
        header = newer / "include/morph/ext.h"
        header.write_text(header.read_text().replace('"1.1.0"', '"1.1.1"'))
        metadata = json.loads((newer / "manifest.json").read_text())
        metadata["sdk_version"] = "1.1.1"
        metadata["files"]["include/morph/ext.h"] = hashlib.sha256(header.read_bytes()).hexdigest()
        (newer / "manifest.json").write_text(json.dumps(metadata))
        self.run_script("check", self.plugin)
        self.run_script("sdk", self.plugin, "--sdk-version", "1.1.1", success=False)
        pinned = self.plugin / "sdk/manifest.json"
        self.assertEqual(json.loads(pinned.read_text())["sdk_version"], "1.1.0")
        self.run_script("sdk", self.plugin, "--sdk-version", "1.1.1", "--replace")
        self.run_script("check", self.plugin)
        self.assertEqual(json.loads(pinned.read_text())["sdk_version"], "1.1.1")
        self.assertEqual(json.loads((old / "manifest.json").read_text())["sdk_version"], "1.1.0")

    def test_incomplete_skill_package_fails_before_creating_a_project(self):
        (self.skill / "assets/native/plugin.c.tmpl").unlink()
        result = self.run_script("init", self.plugin, success=False)
        self.assertIn("missing native scaffold templates", result.stderr)
        self.assertFalse(self.plugin.exists())

    def test_file_hash_metadata_cannot_escape_the_sdk_directory(self):
        lock = self.skill / "assets/sdk/1.1.0/manifest.json"
        metadata = json.loads(lock.read_text())
        metadata["files"]["../outside"] = "0" * 64
        lock.write_text(json.dumps(metadata))
        self.run_script("init", self.plugin, success=False)
        self.assertFalse(self.plugin.exists())

    def test_packaged_release_matches_public_host_headers(self):
        version = re.search(r'^#define EXT_SDK_VERSION "([^"]+)"$',
                            (PUBLIC / "morph/ext.h").read_text(), re.M)[1]
        bundle = self.skill / "assets/sdk" / version
        metadata = json.loads((bundle / "manifest.json").read_text())
        self.assertIn(f"{version} (ABI {metadata['abi_version']})", self.run_script("versions").stdout)
        for name in ("ext.h", "ext_types.h"):
            self.assertEqual((bundle / "include/morph" / name).read_bytes(),
                             (PUBLIC / "morph" / name).read_bytes(),
                             "Public SDK changed without a new bundled release")
        for name, digest in metadata["files"].items():
            self.assertEqual(hashlib.sha256((bundle / name).read_bytes()).hexdigest(), digest)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--skill", type=Path, default=SKILL)
    parser.add_argument("--public-include", type=Path, default=PUBLIC)
    parser.add_argument("--cc", default=CC)
    args, remaining = parser.parse_known_args()
    SKILL = args.skill.resolve()
    PUBLIC = args.public_include.resolve()
    CC = args.cc
    unittest.main(argv=[sys.argv[0], *remaining])
