#!/usr/bin/env python3

import json
import subprocess
import sys


def run(binary, *args):
    return subprocess.run(
        [binary, *args],
        check=False,
        capture_output=True,
        text=True,
    )


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]

    help_result = run(binary, "--help")
    require(help_result.returncode == 0, help_result.stderr)
    require(help_result.stdout.startswith("Usage: morph [options]\n"),
            help_result.stdout)
    require("--events json" in help_result.stdout, help_result.stdout)

    event_result = run(binary, "--events=json", "--help")
    require(event_result.returncode == 0, event_result.stderr)
    events = [json.loads(line) for line in event_result.stdout.splitlines()]
    require(len(events) == 2, event_result.stdout)
    require(events[0]["name"] == "command.started", event_result.stdout)
    require(events[1]["name"] == "command.completed", event_result.stdout)
    require(events[1]["data"]["output"] == help_result.stdout,
            event_result.stdout)

    invalid_mode = run(binary, "--events", "yaml")
    require(invalid_mode.returncode == 2, invalid_mode.stderr)
    require("invalid --events mode" in invalid_mode.stderr,
            invalid_mode.stderr)

    missing_value = run(binary, "--config")
    require(missing_value.returncode == 2, missing_value.stderr)
    require("option requires an argument" in missing_value.stderr,
            missing_value.stderr)

    unknown = run(binary, "--not-an-option")
    require(unknown.returncode == 2, unknown.stderr)
    require("unknown option" in unknown.stderr, unknown.stderr)

    positional = run(binary, "unexpected")
    require(positional.returncode == 2, positional.stderr)
    require("unexpected argument" in positional.stderr, positional.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
