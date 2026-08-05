#!/usr/bin/env python3
"""JSON-RPC wrapper for ripgrep."""

import json
import os
import selectors
import shutil
import subprocess
import sys


DEFAULT_MAX_OUTPUT_BYTES = 256 * 1024
HARD_MAX_OUTPUT_BYTES = 1024 * 1024
MAX_PATHS = 64
MAX_GLOBS = 32


class InvalidParams(ValueError):
	pass


def require_string(params, name):
	value = params.get(name)
	if not isinstance(value, str) or not value:
		raise InvalidParams(f"{name} must be a non-empty string")
	return value


def string_list(params, name, limit):
	value = params.get(name, [])
	if not isinstance(value, list) or len(value) > limit:
		raise InvalidParams(f"{name} must be an array with at most {limit} items")
	if any(not isinstance(item, str) or not item for item in value):
		raise InvalidParams(f"every {name} item must be a non-empty string")
	return value


def boolean(params, name, default=False):
	value = params.get(name, default)
	if not isinstance(value, bool):
		raise InvalidParams(f"{name} must be a boolean")
	return value


def bounded_integer(params, name, default, minimum, maximum):
	value = params.get(name, default)
	if isinstance(value, bool) or not isinstance(value, int):
		raise InvalidParams(f"{name} must be an integer")
	if value < minimum or value > maximum:
		raise InvalidParams(f"{name} must be between {minimum} and {maximum}")
	return value


def build_command(params):
	pattern = require_string(params, "pattern")
	paths = string_list(params, "paths", MAX_PATHS) or ["."]
	globs = string_list(params, "globs", MAX_GLOBS)
	case_mode = params.get("case", "smart")
	mode = params.get("mode", "content")

	if case_mode not in ("smart", "sensitive", "insensitive"):
		raise InvalidParams("case must be smart, sensitive, or insensitive")
	if mode not in ("content", "files", "count"):
		raise InvalidParams("mode must be content, files, or count")

	command = ["rg", "--no-config", "--color=never"]
	if case_mode == "smart":
		command.append("--smart-case")
	elif case_mode == "sensitive":
		command.append("--case-sensitive")
	else:
		command.append("--ignore-case")

	if mode == "files":
		command.append("--files-with-matches")
	elif mode == "count":
		command.append("--count")
	else:
		command.extend(("--line-number", "--no-heading"))

	if boolean(params, "fixed_strings"):
		command.append("--fixed-strings")
	if boolean(params, "word_regexp"):
		command.append("--word-regexp")
	if boolean(params, "hidden"):
		command.append("--hidden")
	if boolean(params, "follow"):
		command.append("--follow")

	context = bounded_integer(params, "context", 0, 0, 100)
	max_count = bounded_integer(params, "max_count", 0, 0, 10000)
	if context:
		command.extend(("--context", str(context)))
	if max_count:
		command.extend(("--max-count", str(max_count)))
	for glob in globs:
		command.extend(("--glob", glob))

	command.extend(("--", pattern, *paths))
	return command


def collect_output(process, limit):
	selector = selectors.DefaultSelector()
	buffers = {"stdout": bytearray(), "stderr": bytearray()}
	truncated = False

	for name, stream in (("stdout", process.stdout), ("stderr", process.stderr)):
		os.set_blocking(stream.fileno(), False)
		selector.register(stream, selectors.EVENT_READ, name)

	while selector.get_map():
		for key, _ in selector.select():
			chunk = os.read(key.fileobj.fileno(), 65536)
			if not chunk:
				selector.unregister(key.fileobj)
				key.fileobj.close()
				continue
			used = len(buffers["stdout"]) + len(buffers["stderr"])
			remaining = max(0, limit - used)
			buffers[key.data].extend(chunk[:remaining])
			if len(chunk) > remaining:
				truncated = True

	return (
		buffers["stdout"].decode("utf-8", errors="replace"),
		buffers["stderr"].decode("utf-8", errors="replace"),
		truncated,
	)


def run_rg(params):
	if not isinstance(params, dict):
		raise InvalidParams("params must be an object")
	if shutil.which("rg") is None:
		raise RuntimeError("rg executable was not found in PATH")

	limit = bounded_integer(
		params,
		"max_output_bytes",
		DEFAULT_MAX_OUTPUT_BYTES,
		1,
		HARD_MAX_OUTPUT_BYTES,
	)
	command = build_command(params)
	process = subprocess.Popen(
		command,
		stdin=subprocess.DEVNULL,
		stdout=subprocess.PIPE,
		stderr=subprocess.PIPE,
	)
	stdout, stderr, truncated = collect_output(process, limit)
	exit_code = process.wait()
	return {
		"stdout": stdout,
		"stderr": stderr,
		"exit_code": exit_code,
		"matched": exit_code == 0,
		"truncated": truncated,
	}


def response(request):
	request_id = request.get("id") if isinstance(request, dict) else None
	try:
		if not isinstance(request, dict) or request.get("method") != "run":
			raise InvalidParams("method must be run")
		return {
			"jsonrpc": "2.0",
			"id": request_id,
			"result": run_rg(request.get("params")),
		}
	except InvalidParams as error:
		return {
			"jsonrpc": "2.0",
			"id": request_id,
			"error": {"code": -32602, "message": str(error)},
		}
	except (OSError, RuntimeError) as error:
		return {
			"jsonrpc": "2.0",
			"id": request_id,
			"error": {"code": -32000, "message": str(error)},
		}


def main():
	line = sys.stdin.readline()
	try:
		request = json.loads(line)
	except (json.JSONDecodeError, UnicodeDecodeError) as error:
		result = {
			"jsonrpc": "2.0",
			"id": None,
			"error": {"code": -32700, "message": f"parse error: {error}"},
		}
	else:
		result = response(request)
	json.dump(result, sys.stdout, ensure_ascii=False, separators=(",", ":"))
	sys.stdout.write("\n")


if __name__ == "__main__":
	main()
