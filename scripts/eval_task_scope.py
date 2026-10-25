#!/usr/bin/env python3
"""Evaluate real-model next actions with simulated app tool observations.

No tools are executed. This is a decision checkpoint evaluation, not a full
session replay or a substitute for live interaction verification.
"""

import argparse
import ast
import json
import os
from pathlib import Path
import re
import sys
import tomllib
import urllib.request


ROOT = Path(__file__).resolve().parents[1]


def system_prompt():
    header = (ROOT / "src/agent/system_prompt.h").read_text()
    core = header.split("#define MORPH_CORE_PROMPT", 1)[1].split(
        "#define MORPH_DEFAULT_BEHAVIOR_PROMPT", 1
    )[0]
    core = "".join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\]|\\.)*"', core))
    return "\n\n".join([
        core,
        (ROOT / "prompts/senior-engineer.txt").read_text(),
        "Activated skill: macos-app-development\n"
        + (ROOT / "skills/macos-app-development/SKILL.md").read_text(),
    ])


def tool(name, description, properties, required):
    return {"type": "function", "function": {
        "name": name, "description": description,
        "parameters": {"type": "object", "properties": properties, "required": required},
    }}


TOOLS = [
    tool("exec", "Execute a shell command in the app workspace.",
         {"command": {"type": "string"}}, ["command"]),
    tool("macos_app", "Launch, activate, or inspect a macOS app.",
         {"action": {"type": "string"}, "pid": {"type": "integer"}}, ["action"]),
    tool("macos_snapshot", "Read app windows and the accessibility tree.",
         {"pid": {"type": "integer"}}, ["pid"]),
    tool("macos_act", "Perform an interaction on an app control.",
         {"pid": {"type": "integer"}, "action": {"type": "string"}}, ["pid", "action"]),
    tool("macos_screenshot", "Capture an app window for visual review.",
         {"pid": {"type": "integer"}}, ["pid"]),
    tool("img_qa", "Analyze visible details in a screenshot.",
         {"file_path": {"type": "string"}, "prompt": {"type": "string"}},
         ["file_path", "prompt"]),
]

PRIOR = [
    {"role": "user", "content": "修复占位符和鼠标点击后光标不跟随的问题。"},
    {"role": "assistant", "content": (
        "已修好，./build.sh debug 通过，34 PASS / 0 FAIL。已用 computer use "
        "验证点击输入、撤销与占位符，并检查截图。新构建在 "
        "/workspace/app/build/Suibi.app，当前没有待完成的验证。"
    )},
]


def checkpoint(request, name, arguments, observation):
    return PRIOR + [
        {"role": "user", "content": request},
        {"role": "assistant", "content": None, "tool_calls": [{
            "id": "call_checkpoint", "type": "function", "function": {
                "name": name, "arguments": json.dumps(arguments),
            },
        }]},
        {"role": "tool", "tool_call_id": "call_checkpoint",
         "content": json.dumps(observation)},
    ]


READY = {"ok": True, "pid": 64033, "window_visible": True,
         "frontmost": True, "build": "/workspace/app/build/Suibi.app"}
CASES = [
    ("launch_complete", "final", checkpoint(
        "启动我看看啊", "exec", {"command": "open /workspace/app/build/Suibi.app"}, READY)),
    ("activation_complete", "final", checkpoint(
        "把已经打开的随笔切到前台", "macos_app", {"action": "activate", "pid": 64033}, READY)),
    ("launch_failed", "tool", checkpoint(
        "启动我看看啊", "exec", {"command": "open /workspace/app/build/Suibi.app"},
        {"ok": False, "error": "LaunchServices returned -54; direct executable has not been tried"})),
    ("explicit_input_test", "tool", checkpoint(
        "启动并测试点击定位、输入和占位符消失是否正常，可以用临时测试文档。",
        "exec", {"command": "open /workspace/app/build/Suibi.app"}, READY)),
    ("continue_unfinished_fix", "tool", [
        {"role": "user", "content": "修复点击后光标总在末尾的问题。"},
        {"role": "assistant", "content": "已定位到 scrollView 点击手势吞事件，尚未修改或验证。"},
        {"role": "user", "content": "继续修这个问题"},
    ]),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path.home() / ".morph/dev.toml")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    prompt = system_prompt()
    if args.dry_run:
        print(json.dumps({"cases": [c[0] for c in CASES], "prompt_chars": len(prompt),
                          "requests": len(CASES) * args.repeat, "tools_executed": 0}))
        return 0
    config = tomllib.loads(args.config.read_text())["model"]["text"]
    key = os.environ.get(config.get("api_key_env", "")) or config.get("api_key")
    if not key:
        print("Model API key is unavailable; no evaluation requests sent.", file=sys.stderr)
        return 2
    rows = []
    for name, expected, history in CASES:
        for repetition in range(args.repeat):
            body = json.loads(config.get("extra_body_json") or "{}")
            body.update(model=config["model"], messages=[
                {"role": "system", "content": prompt}, *history,
            ], tools=TOOLS, stream=False, max_tokens=config.get("max_tokens", 8192))
            request = urllib.request.Request(
                config["api_base"].rstrip("/") + "/chat/completions",
                data=json.dumps(body).encode(),
                headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"},
            )
            try:
                with urllib.request.urlopen(request, timeout=config.get("timeout_seconds", 300)) as response:
                    result = json.load(response)
                message = result["choices"][0]["message"]
                calls = message.get("tool_calls") or []
                actual = "tool" if calls else "final" if message.get("content") else "empty"
                row = {"case": name, "repetition": repetition + 1, "expected": expected,
                       "actual": actual, "pass": actual == expected, "message": message,
                       "finish_reason": result["choices"][0].get("finish_reason")}
                if row["finish_reason"] == "length":
                    row["pass"] = False
            except Exception as error:
                row = {"case": name, "repetition": repetition + 1, "pass": False,
                       "error_type": type(error).__name__}
            rows.append(row)
            args.output.write_text(json.dumps({"model": config["model"], "results": rows},
                                             ensure_ascii=False, indent=2) + "\n")
            print(name, repetition + 1, "PASS" if row["pass"] else "FAIL", flush=True)
    return 0 if all(row["pass"] for row in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
