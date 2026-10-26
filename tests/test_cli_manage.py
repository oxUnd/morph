#!/usr/bin/env python3
"""Exercise management commands against isolated configs and local Git repos."""

import concurrent.futures
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import pty
import shutil
import stat
import subprocess
import sys
import tempfile
import threading

try:
    import tomllib
except ModuleNotFoundError:
    print("Management integration needs Python 3.11+ (tomllib).")
    sys.exit(77)


BINARY = str(Path(sys.argv[1]).resolve())


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Workspace:
    def __init__(self, root):
        self.root = root
        self.home = root / "home"
        self.project = root / "project"
        self.home.mkdir()
        self.project.mkdir()
        self.env = dict(os.environ, HOME=str(self.home), GIT_CONFIG_NOSYSTEM="1",
                        GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0",
                        NO_PROXY="localhost,127.0.0.1,::1",
                        no_proxy="localhost,127.0.0.1,::1")

    def morph(self, *args, ok=True, cwd=None, env=None):
        result = subprocess.run([BINARY, *map(str, args)], cwd=cwd or self.project,
                                env=env or self.env, text=True, capture_output=True,
                                timeout=30)
        require((result.returncode == 0) == ok,
                f"{args}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
        return result

    def git(self, repo, *args):
        result = subprocess.run(["git", "-c", "user.name=Morph Test", "-c",
                                 "user.email=test@example.invalid", *args], cwd=repo,
                                env=self.env, text=True, capture_output=True,
                                timeout=30)
        require(result.returncode == 0, result.stderr)
        return result.stdout.strip()


def make_skill(path, name, body="Version one."):
    path.mkdir(parents=True, exist_ok=True)
    (path / "SKILL.md").write_text(
        f"---\nname: {name}\ndescription: Test {name}.\n---\n{body}\n")
    (path / "references").mkdir(exist_ok=True)
    (path / "references" / "guide.txt").write_text("Resource fixture.\n")
    script = path / "run.sh"
    script.write_text("#!/bin/sh\nprintf 'fixture\\n'\n")
    script.chmod(0o755)


def test_mcp(work):
    config = work.home / ".morph" / "config.toml"
    work.morph("mcp", "list")
    for group in ("mcp", "skills", "skill"):
        work.morph(group, "-h")
        work.morph(group, "add", "--help")
    require(not config.exists(), "Read-only listing created a config")
    work.morph("mcp", "add", "docs", "--url", "https://example.invalid/mcp",
               "--bearer-token-env-var", "MCP_TOKEN")
    parsed = tomllib.loads(config.read_text())
    require(parsed["mcp"]["servers"][0]["auth_token_env"] == "MCP_TOKEN", parsed)
    require(stat.S_IMODE(config.stat().st_mode) == 0o600, "Config must be private")
    original = config.read_bytes()
    work.morph("mcp", "add", "docs", "--url", "https://other.invalid", ok=False)
    require(config.read_bytes() == original, "Duplicate changed config")
    work.morph("mcp", "add", "bad", "--transport", "stdio", "--url",
               "https://example.invalid", ok=False)
    work.morph("mcp", "add", "bad", "--url", "ftp://example.invalid", ok=False)
    work.morph("mcp", "add", "bad", "--", ok=False)
    work.morph("mcp", "add", "bad", "--", "echo", "x" * 256, ok=False)
    work.morph("mcp", "add", "bad", "--scope", "project", "--", "echo", ok=False)
    work.morph("mcp", "add", "bad", "--url", ok=False)
    work.morph("mcp", "add", "bad", "--unknown", ok=False)
    require(config.read_bytes() == original, "Invalid input changed config")

    for name in ("0TOKEN", "MCP-TOKEN", "MCP.TOKEN"):
        work.morph("mcp", "add", "bad-env", "--url", "https://example.invalid/mcp",
                   "--bearer-token-env-var", name, ok=False)
        work.morph("mcp", "add", "bad-env", "--env", name + "=value",
                   "--", "echo", ok=False)
    require(config.read_bytes() == original, "Invalid environment names changed config")

    marker = work.root / "server-started"
    server = work.root / "mock_mcp.py"
    server.write_text('''import json, os, sys
open(os.environ["MORPH_TEST_MARKER"], "w").write("started")
for line in sys.stdin:
    request = json.loads(line)
    if "id" not in request:
        continue
    if request["method"] != "initialize":
        print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "error": {
            "code": -32601, "message": "Method not found"}}), flush=True)
        continue
    result = {"protocolVersion": "2024-11-05", "capabilities": {},
              "serverInfo": {"name": "fixture", "version": "1"}}
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
''')
    work.morph("mcp", "add", "fixture", f"--env=MORPH_TEST_MARKER={marker}",
               "--env", "SECRET=sensitive-value", "--", sys.executable, server,
               "--flag", "space value", 'a"b\\c', "$(touch SHOULD_NOT_EXIST)")
    listing = work.morph("mcp", "list")
    details = work.morph("mcp", "get", "fixture")
    require("fixture" in listing.stdout and "docs" in listing.stdout, listing.stdout)
    require("<redacted>" in details.stdout and "sensitive-value" not in details.stdout,
            details.stdout)
    require(not marker.exists(), "list/get launched the server")
    parsed = tomllib.loads(config.read_text())
    require(parsed["mcp"]["servers"][1]["args"][-1] == "$(touch SHOULD_NOT_EXIST)", parsed)
    work.morph("mcp", "test", "fixture")
    require(marker.exists(), "test did not launch the server")
    require(not (work.project / "SHOULD_NOT_EXIST").exists(), "Server args reached shell")
    work.morph("mcp", "get", "missing", ok=False)

    class HttpMcp(BaseHTTPRequestHandler):
        authenticated = False

        def do_POST(self):
            request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            HttpMcp.authenticated = self.headers.get("Authorization") == "Bearer fixture-token"
            if "id" not in request:
                self.send_response(202)
                self.end_headers()
                return
            if request["method"] == "initialize":
                payload = {"result": {
                    "protocolVersion": request["params"]["protocolVersion"],
                    "capabilities": {}, "serverInfo": {"name": "http-fixture", "version": "1"}
                }}
                status = 200
            else:
                payload = {"error": {"code": -32601, "message": "Method not found"}}
                status = 400
            response = json.dumps({"jsonrpc": "2.0", "id": request["id"], **payload}).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)

        def log_message(self, *args):
            pass

    with ThreadingHTTPServer(("127.0.0.1", 0), HttpMcp) as http:
        thread = threading.Thread(target=http.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://127.0.0.1:{http.server_port}/mcp"
            work.morph("mcp", "add", "http", "--transport=http", url,
                       "--auth-token-env=MORPH_TEST_TOKEN")
            env = dict(work.env, MORPH_TEST_TOKEN="fixture-token")
            require("Connected to http-fixture" in work.morph("mcp", "test", "http", env=env).stdout,
                    "HTTP handshake failed")
            require(HttpMcp.authenticated, "HTTP token environment was not resolved")
        finally:
            http.shutdown()
            thread.join(timeout=5)

    # Preserve unrelated text, quoted headers, nested env, and multiline strings.
    content = '''# User configuration
[general]
default_session = """example
[[mcp.servers]]
This is string data.
"""

[[ "mcp" . "servers" ]]
name = "first"
transport = "stdio"
command = "echo"
[mcp.servers.env]
FIRST = "one"

# Keep this section comment.
[skill]
dir = ""

[[mcp.servers]]
name = "second"
transport = "http"
url = "https://second.invalid/mcp"

# Keep the EOF comment.
'''
    config.write_text(content)
    work.morph("mcp", "remove", "first")
    actual = config.read_text()
    parsed = tomllib.loads(actual)
    require([s["name"] for s in parsed["mcp"]["servers"]] == ["second"], actual)
    require(parsed["general"]["default_session"].startswith("example\n[[mcp.servers]]"), actual)
    require("# Keep this section comment." in actual and "# Keep the EOF comment." in actual,
            actual)
    work.morph("mcp", "add", "second", "--replace", "--transport", "http",
               "https://replacement.invalid/mcp")
    require(tomllib.loads(config.read_text())["mcp"]["servers"][0]["url"] ==
            "https://replacement.invalid/mcp", config.read_text())
    work.morph("mcp", "remove", "second")
    require("servers" not in tomllib.loads(config.read_text()).get("mcp", {}), config.read_text())
    work.morph("mcp", "remove", "second", ok=False)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda i: work.morph("mcp", "add", f"parallel-{i}", "--", "echo", str(i)),
                      range(4)))
    require(len(tomllib.loads(config.read_text())["mcp"]["servers"]) == 4,
            "Concurrent config update was lost")

    custom = work.root / "custom.toml"
    custom.write_text("# Custom config\n")
    work.morph("-c", custom, "mcp", "add", "custom", "--", "echo")
    require("custom" in work.morph("mcp", "list", "-c", custom).stdout, custom.read_text())
    require("custom" not in work.morph("mcp", "list").stdout, "Custom config leaked")
    symlink = work.root / "symlink.toml"
    symlink.symlink_to(custom)
    work.morph("-c", symlink, "mcp", "add", "unsafe", "--", "echo", ok=False)
    broken = work.root / "broken.toml"
    broken.write_text("[broken\n")
    work.morph("-c", broken, "mcp", "add", "invalid", "--", "echo", ok=False)
    require(broken.read_text() == "[broken\n", "Invalid config was overwritten")


def test_skills(work):
    repo = work.root / "skill repo"
    repo.mkdir()
    first = repo / "skills" / "directory-one"
    second = repo / "skills" / "directory-two"
    make_skill(first, "review")
    make_skill(second, "testing")
    (first / "guide-link").symlink_to("references/guide.txt")
    work.git(repo, "init", "--quiet", "--initial-branch=main")
    work.git(repo, "add", ".")
    work.git(repo, "commit", "--quiet", "-m", "Initial skills")
    pinned = work.git(repo, "rev-parse", "HEAD")
    work.git(repo, "tag", "v1")

    install_root = work.project / ".agents" / "skills"
    listing = work.morph("skills", "add", repo, "--list")
    require("review" in listing.stdout and "testing" in listing.stdout, listing.stdout)
    require(not install_root.exists(), "Source listing created install directory")
    work.morph("skills", "add", repo, "-y", ok=False)
    require(not (install_root / "review").exists(), "--yes selected every skill")
    work.morph("skills", "add", repo, "--skill", "absent", ok=False)
    work.morph("skills", "add", repo, "--path", "../..", ok=False)
    work.morph("skills", "add", repo, "--ref", "--bad", ok=False)
    work.morph("skills", "add", repo, "--skill=review")
    installed = install_root / "review"
    require((installed / "references" / "guide.txt").read_text() == "Resource fixture.\n",
            "Resources were not copied")
    require(os.access(installed / "run.sh", os.X_OK), "Executable bit was lost")
    require((installed / "guide-link").is_symlink(), "Internal link was not preserved")
    metadata = json.loads((installed / ".morph-install.json").read_text())
    require(metadata["commit"] == pinned and metadata["path"] == "skills/directory-one", metadata)
    require(not (install_root / "testing").exists(), "Unselected skill was installed")
    work.morph("skills", "add", repo, "--skill", "review", ok=False)
    work.morph("skills", "add", repo, "--skill", "review", "--replace")

    # Same name can coexist at project and user scope. Pin global to a commit.
    work.morph("skill", "add", repo, "--path", "skills/directory-one", "--ref", pinned, "-g")
    global_review = work.home / ".morph" / "skills" / "review"
    require(global_review.exists(), "Global installation missing")
    effective = work.morph("skills", "list").stdout
    review_lines = [line for line in effective.splitlines() if line.startswith("review\t")]
    require(len(review_lines) == 1 and str(installed) in review_lines[0], effective)
    require(str(global_review) in work.morph("skills", "list", "-g").stdout,
            "Global list included wrong scope")
    shared = work.home / ".agents" / "skills" / "shared"
    make_skill(shared, "shared")
    require("shared\t" in work.morph("skills", "list").stdout, "Shared user skills missing")

    (first / "SKILL.md").write_text((first / "SKILL.md").read_text().replace("Version one", "Version two"))
    work.git(repo, "add", ".")
    work.git(repo, "commit", "--quiet", "-m", "Update review")
    work.morph("skills", "update", "review")
    require("Version two" in (installed / "SKILL.md").read_text(), "Branch update did not apply")
    work.morph("skills", "update", "review", "-g")
    require("Version one" in (global_review / "SKILL.md").read_text(), "Pinned commit moved")
    work.morph("skills", "add", repo, "--path", "skills/directory-two", "--ref", "v1")
    work.morph("skills", "update")

    original = (installed / "SKILL.md").read_bytes()
    (installed / "references" / "guide.txt").write_text("Local edits")
    before = (installed / ".morph-install.json").read_bytes()
    work.morph("skills", "update", "review", ok=False)
    work.morph("skills", "remove", "review", ok=False)
    work.morph("skills", "add", repo, "--skill", "review", "--replace", ok=False)
    require((installed / "SKILL.md").read_bytes() == original and
            (installed / ".morph-install.json").read_bytes() == before,
            "Modified installation was overwritten")
    (installed / "references" / "guide.txt").write_text("Resource fixture.\n")
    work.morph("skills", "remove", "review", "testing")
    require(not installed.exists() and global_review.exists(), "Removal crossed scope")

    local = work.root / "local"
    make_skill(local, "local")
    work.morph("skills", "add", local)
    (local / "SKILL.md").write_text((local / "SKILL.md").read_text() + "New local content.\n")
    work.morph("skills", "update", "local")
    require("New local content." in (install_root / "local" / "SKILL.md").read_text(),
            "Local source update failed")

    # Source with an escaping symlink must leave the current installation intact.
    (local / "escape").symlink_to(work.root / "custom.toml")
    work.morph("skills", "update", "local", ok=False)
    require((install_root / "local" / "SKILL.md").exists(), "Failed update destroyed original")
    (local / "escape").unlink()
    work.morph("skills", "remove", "local")

    unmanaged = install_root / "unmanaged"
    make_skill(unmanaged, "unmanaged")
    work.morph("skills", "remove", "unmanaged", ok=False)
    work.morph("skills", "update")  # No-argument update skips unmanaged skills.
    work.morph("skills", "remove", "../escape", ok=False)

    custom_root = work.root / "custom-skills"
    config = work.root / "skill-config.toml"
    config.write_text(f"[skill]\ndir = {json.dumps(str(custom_root))}\n")
    work.morph("-c", config, "skills", "add", repo.as_uri(), "--all", "-g")
    require((custom_root / "review").exists() and (custom_root / "testing").exists(),
            "Custom global directory ignored")
    work.morph("-c", config, "skills", "update", "-g")
    work.morph("-c", config, "skills", "remove", "review", "testing", "-g")

    # Standard short-option grouping and multiple names after --skill.
    work.morph("-c", config, "skills", "add", repo, "--skill", "review", "testing", "-gy")
    work.morph("-c", config, "skills", "remove", "review", "testing", "-g")

    # Exercise the interactive selection path without external PTY dependencies.
    master, slave = pty.openpty()
    try:
        process = subprocess.Popen([BINARY, "skills", "add", str(repo)],
                                   cwd=work.project, env=work.env, stdin=slave,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True)
        os.close(slave)
        slave = -1
        os.write(master, b"1\n")
        stdout, stderr = process.communicate(timeout=30)
        require(process.returncode == 0 and "Select skill numbers" in stderr,
                stdout + stderr)
        require((install_root / "review").exists() and not (install_root / "testing").exists(),
                "Interactive selection installed the wrong skills")
    finally:
        if slave >= 0:
            os.close(slave)
        os.close(master)
    work.morph("skills", "remove", "review")

    # -w selects the same project for management and runtime discovery.
    another = work.root / "another"
    another.mkdir()
    work.morph("-w", another, "skills", "add", local)
    require((another / ".agents" / "skills" / "local").exists(), "-w was ignored")
    work.morph("-w", another, "skills", "remove", "local")
    require(not list(install_root.glob(".morph-install-*")), "Staging directories leaked")
    require(not list((work.home / ".morph" / "cache" / "skills").iterdir()),
            "Temporary Git checkouts leaked")


def main():
    if not shutil.which("git"):
        return 77
    with tempfile.TemporaryDirectory(prefix="morph-cli-manage-") as temporary:
        work = Workspace(Path(temporary).resolve())
        test_mcp(work)
        test_skills(work)
    return 0


if __name__ == "__main__":
    sys.exit(main())
