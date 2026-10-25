#!/usr/bin/env python3
"""Add one bounded open-and-verify operation to macOS Computer Use Kit MCP.

All actions pass through the upstream MCP server, including its policy gates.
No third-party source is patched. The low-level tools remain available.
"""

import argparse
import json
import subprocess
import sys


OPEN_TOOL = {
    "name": "macos_open_app",
    "description": (
        "Preferred for requests to open or bring a macOS app into view. Launches, "
        "activates by resolved PID, and checks its on-screen windows in one operation. "
        "When completed=true, the launch-only request is satisfied: respond immediately; "
        "do not add screenshots, OCR, focus, or UI tests. Keeps upstream policy refusals."
    ),
    "inputSchema": {
        "type": "object", "additionalProperties": False,
        "properties": {"app": {
            "type": "string", "minLength": 1,
            "description": "Application name or bundle ID, preferably a known bundle ID.",
        }},
        "required": ["app"],
    },
    "annotations": {
        "readOnlyHint": False, "destructiveHint": False,
        "idempotentHint": False, "openWorldHint": False,
    },
}


def result(payload, error=False):
    return {"content": [{"type": "text", "text": json.dumps(payload, ensure_ascii=False)}],
            "isError": error}


def decode(reply):
    if "error" in reply:
        raise ValueError("upstream MCP request failed")
    body = reply["result"]
    text = "\n".join(c["text"] for c in body.get("content", []) if c.get("type") == "text")
    payload = json.loads(text)
    if not isinstance(payload, dict):
        raise ValueError("upstream tool returned a non-object result")
    return payload, bool(body.get("isError"))


class Adapter:
    def __init__(self, exchange):
        self.exchange = exchange
        self.sequence = 0
        self.available = False

    def call(self, name, arguments):
        self.sequence += 1
        return decode(self.exchange({
            "jsonrpc": "2.0", "id": f"morph-open-{self.sequence}",
            "method": "tools/call", "params": {"name": name, "arguments": arguments},
        }))

    def open_app(self, arguments):
        app = arguments.get("app")
        if set(arguments) != {"app"} or not isinstance(app, str) or not app.strip():
            return result({"ok": False, "reason": "invalid_arguments", "retry": "never"}, True)
        steps = []

        def run(name, args):
            payload, failed = self.call(name, args)
            steps.append({"tool": name, "arguments": args, "result": payload})
            return payload, failed

        def finish(payload, failed=True):
            return result({**payload, "completed": False, "steps": steps}, failed)

        launched, failed = run("macos_app", {"action": "launch", "app": app})
        if launched.get("dry_run"):
            return finish(launched, False)
        pid = launched.get("pid")
        if failed or not launched.get("ok") or not pid:
            if launched.get("reason") != "launch_timeout":
                return finish(launched)
            listing, list_failed = run("macos_app", {"action": "list"})
            if list_failed:
                return finish(listing)
            needle = app.casefold().removesuffix(".app")
            candidates = [a for a in listing.get("apps", []) if needle in {
                str(a.get("name", "")).casefold(),
                str(a.get("bundle", "")).casefold(),
                str(a.get("bundle", "")).rsplit(".", 1)[-1].casefold(),
            }]
            if len(candidates) != 1 or not candidates[0].get("pid"):
                return finish(launched)
            pid = candidates[0]["pid"]
        activated, failed = run("macos_app", {"action": "activate", "pid": pid})
        if failed or not activated.get("ok") or activated.get("verified") is not True:
            if failed or activated.get("reason") or activated.get("dry_run"):
                return finish(activated)
            return finish({**activated, "ok": False, "reason": "activation_not_verified",
                           "retry": "reobserve"})
        windows, failed = run("macos_window", {"action": "list", "pid": pid})
        if failed:
            return finish({**windows, "activation_verified": True, "pid": pid})
        visible = [w for w in windows.get("windows", []) if w.get("pid") == pid]
        if not visible:
            return finish({"ok": False, "reason": "window_not_visible", "retry": "reobserve",
                           "activation_verified": True, "pid": pid})
        return result({"ok": True, "completed": True, "action": "open_app", "pid": pid,
                       "name": activated.get("name"), "bundle": activated.get("bundle"),
                       "activation_verified": True, "window_visible": True,
                       "windows": visible, "steps": steps})

    def handle(self, message):
        method = message.get("method")
        if method == "tools/call" and message.get("params", {}).get("name") == OPEN_TOOL["name"]:
            try:
                if not self.available:
                    raise ValueError("open_app is unavailable in the upstream tool selection")
                body = self.open_app(message["params"].get("arguments") or {})
            except (KeyError, TypeError, ValueError) as exc:
                body = result({"ok": False, "completed": False,
                               "reason": "adapter_protocol_error", "detail": str(exc)}, True)
            return {"jsonrpc": "2.0", "id": message.get("id"), "result": body}
        reply = self.exchange(message)
        params = message.get("params") or {}
        if (self.available and method == "tools/call" and params.get("name") == "macos_app"
                and (params.get("arguments") or {}).get("action") == "activate"):
            payload, failed = decode(reply)
            if not failed and payload.get("verified") is True and payload.get("pid"):
                windows, window_failed = self.call("macos_window", {
                    "action": "list", "pid": payload["pid"],
                })
                visible = [w for w in windows.get("windows", [])
                           if w.get("pid") == payload["pid"]] if not window_failed else []
                payload["foreground_check"] = {
                    "completed": bool(visible), "windows": visible,
                    "observation": windows,
                    "scope": "App activation and visible window only; not broader task completion.",
                    "next_step": ("For a launch/foreground-only request, respond now. "
                                  "Do not add another window, focus, screenshot, or point check. "
                                  "For a UI test, continue only the requested interactions.")
                    if visible else "Visible-window confirmation is unavailable.",
                }
                reply["result"] = result(payload)
        if method == "tools/list" and reply and "result" in reply:
            specs = reply["result"]["tools"]
            by_name = {t["name"]: t for t in specs}
            self.available = (
                "macos_app" in by_name and "macos_window" in by_name
                and by_name["macos_app"].get("annotations", {}).get("readOnlyHint") is False
            )
            if self.available:
                by_name["macos_app"]["description"] = (
                    "For launch/foreground-only requests prefer macos_open_app. "
                    "Activation returns foreground_check evidence; when completed=true, "
                    "finish that request without more UI probes. "
                    + by_name["macos_app"].get("description", "")
                )
                specs.insert(0, OPEN_TOOL)
        return reply


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("supply the upstream MCP command after --")
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          text=True, bufsize=1) as child:
        def exchange(message):
            child.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
            child.stdin.flush()
            if "id" not in message:
                return None
            line = child.stdout.readline()
            if not line:
                raise ValueError("upstream MCP closed its output")
            reply = json.loads(line)
            if reply.get("id") != message["id"]:
                raise ValueError("upstream MCP response ID mismatch")
            return reply

        adapter = Adapter(exchange)
        try:
            for line in sys.stdin:
                message = None
                try:
                    message = json.loads(line)
                    if not isinstance(message, dict):
                        raise ValueError("expected a JSON-RPC message object")
                    reply = adapter.handle(message)
                except (ValueError, TypeError, BrokenPipeError) as exc:
                    reply = {"jsonrpc": "2.0", "id": message.get("id")
                             if isinstance(message, dict) else None,
                             "error": {"code": -32603, "message": str(exc)}}
                if reply is not None:
                    print(json.dumps(reply, ensure_ascii=False), flush=True)
        finally:
            child.stdin.close()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.terminate()
                child.wait(timeout=5)
    return 0


if __name__ == "__main__":
    sys.exit(main())
