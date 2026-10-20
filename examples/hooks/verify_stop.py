"""Example Stop hook. Run Morph from the repository root with a configured build."""

import json
import subprocess
import sys


def main():
    event = json.load(sys.stdin)
    if event.get("hook_event_name") != "Stop":
        return 0
    try:
        result = subprocess.run(
            ["ctest", "--test-dir", "build", "--output-on-failure", "-R", r"^ArenaTest\.", "--no-tests=error"],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        print(json.dumps({"decision": "block", "reason": f"Run the focused tests: {error}"}))
        return 0
    if result.returncode:
        details = (result.stdout + result.stderr)[-6000:]
        print(json.dumps({"decision": "block", "reason": f"Fix the focused tests:\n{details}"}))
    else:
        print("{}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
