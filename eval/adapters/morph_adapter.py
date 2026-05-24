"""Adapter to run morph CLI and parse its trace-json output."""

from __future__ import annotations

import json
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


@dataclass
class ToolCall:
    name: str
    args: str


@dataclass
class ReactStep:
    type: str
    content: Optional[str] = None
    tool_name: Optional[str] = None
    tool_args: Optional[str] = None


@dataclass
class EvalResult:
    prompt: str
    final_answer: Optional[str]
    tool_calls: list[ToolCall] = field(default_factory=list)
    steps: list[ReactStep] = field(default_factory=list)
    state: str = "unknown"
    elapsed_seconds: float = 0.0
    raw_output: str = ""
    error: Optional[str] = None


def _parse_trace_json(raw: str) -> dict:
    """Extract the last JSON line from morph stdout (trace-json output)."""
    for line in reversed(raw.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                continue
    return {}


def _extract_tool_calls(steps: list[ReactStep]) -> list[ToolCall]:
    calls = []
    for s in steps:
        if s.type.lower() == "action" and s.tool_name:
            calls.append(ToolCall(name=s.tool_name, args=s.tool_args or "{}"))
    return calls


class MorphAdapter:
    """Run morph CLI in one-shot mode and parse results."""

    def __init__(
        self,
        morph_bin: str | Path = "./build/morph",
        config_path: Optional[str] = None,
        timeout: int = 300,
        home_dir: Optional[str] = None,
    ):
        self.morph_bin = str(Path(morph_bin).resolve())
        self.config_path = config_path
        self.timeout = timeout
        self.home_dir = home_dir

    def run(self, prompt: str) -> EvalResult:
        cmd = [self.morph_bin, "-p", prompt, "--trace-json"]
        if self.config_path:
            cmd.extend(["-c", self.config_path])

        env = None
        if self.home_dir:
            import os
            env = os.environ.copy()
            env["HOME"] = self.home_dir

        start = time.monotonic()
        try:
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=self.timeout,
                env=env,
            )
            elapsed = time.monotonic() - start
            raw = proc.stdout
            trace = _parse_trace_json(raw)
        except subprocess.TimeoutExpired:
            elapsed = time.monotonic() - start
            return EvalResult(
                prompt=prompt,
                final_answer=None,
                state="timeout",
                elapsed_seconds=elapsed,
                error=f"timed out after {self.timeout}s",
            )
        except Exception as exc:
            elapsed = time.monotonic() - start
            return EvalResult(
                prompt=prompt,
                final_answer=None,
                state="error",
                elapsed_seconds=elapsed,
                error=str(exc),
            )

        if not trace:
            return EvalResult(
                prompt=prompt,
                final_answer=None,
                state="parse_error",
                elapsed_seconds=elapsed,
                raw_output=raw,
                error="no trace JSON found in output",
            )

        steps = []
        for s in trace.get("steps", []):
            steps.append(ReactStep(
                type=s.get("type", ""),
                content=s.get("content"),
                tool_name=s.get("tool_name"),
                tool_args=s.get("tool_args"),
            ))

        tool_calls = _extract_tool_calls(steps)

        return EvalResult(
            prompt=prompt,
            final_answer=trace.get("final_answer"),
            tool_calls=tool_calls,
            steps=steps,
            state=trace.get("state", "unknown"),
            elapsed_seconds=elapsed,
            raw_output=raw,
        )

    def run_batch(
        self, prompts: list[str], max_workers: int = 1
    ) -> list[EvalResult]:
        if max_workers <= 1:
            return [self.run(p) for p in prompts]
        from concurrent.futures import ThreadPoolExecutor, as_completed
        results: dict[int, EvalResult] = {}
        with ThreadPoolExecutor(max_workers=max_workers) as pool:
            futures = {pool.submit(self.run, p): i for i, p in enumerate(prompts)}
            for fut in as_completed(futures):
                idx = futures[fut]
                results[idx] = fut.result()
        return [results[i] for i in range(len(prompts))]
