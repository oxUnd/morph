#!/usr/bin/env python3
"""morph eval harness — run evaluation cases against a morph binary."""

from __future__ import annotations

import argparse
import json
import sys
import os
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from adapters.morph_adapter import MorphAdapter
from scorers.tool_match import score_tool_match, aggregate_tool_match
from scorers.tool_match import ToolMatchScore
from scorers.task_pass import score_task_pass, aggregate_task_pass, pass_at_k
from scorers.task_pass import TaskPassScore
from scorers.llm_judge import score_llm_judge, aggregate_llm_judge
from report import print_report


def load_cases(path: str) -> list[dict]:
    cases = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                cases.append(json.loads(line))
    return cases


def run_tool_selection(
    adapter: MorphAdapter,
    cases: list[dict],
    repeat: int = 1,
) -> dict:
    """Run L1 tool selection evaluation."""
    all_scores: list[tuple[str, list[ToolMatchScore]]] = []

    for case in cases:
        cid = case["id"]
        for _ in range(repeat):
            result = adapter.run(case["prompt"])
            scores = score_tool_match(result, case.get("expected_tools", []))
            for s in scores:
                s.case_id = cid
            all_scores.append((cid, scores))

    return aggregate_tool_match(all_scores)


def run_task_completion(
    adapter: MorphAdapter,
    cases: list[dict],
    repeat: int = 1,
    judge: bool = False,
    judge_model: str = "gpt-4o-mini",
) -> dict:
    """Run L2 task completion evaluation."""
    all_scores: list[tuple[str, TaskPassScore]] = []
    per_case_runs: dict[str, list[bool]] = {}
    judge_scores: list[tuple[str, any]] = []

    for case in cases:
        cid = case["id"]
        per_case_runs[cid] = []
        for _ in range(repeat):
            result = adapter.run(case["prompt"])
            score = score_task_pass(
                result,
                expected_tools_sequence=case.get("expected_tools_sequence"),
                expected_answer_contains=case.get("expected_answer_contains"),
            )
            score.case_id = cid
            score.details["category"] = case.get("category", "unknown")
            all_scores.append((cid, score))
            per_case_runs[cid].append(score.passed)

            if judge and result.final_answer:
                js = score_llm_judge(result, model=judge_model)
                js.case_id = cid
                judge_scores.append((cid, js))

    agg = aggregate_task_pass(all_scores)
    if repeat > 1:
        for k in [1, 2, 3]:
            if repeat >= k:
                agg[f"pass_at_{k}"] = pass_at_k(per_case_runs, k)["_overall"]

    if judge:
        agg["llm_judge"] = aggregate_llm_judge(judge_scores)

    return agg


def main():
    parser = argparse.ArgumentParser(description="morph eval harness")
    parser.add_argument(
        "--cases", required=True, help="Path to JSONL case file"
    )
    parser.add_argument(
        "--level",
        choices=["tool_selection", "task_completion"],
        default=None,
        help="Eval level (auto-detected from filename if omitted)",
    )
    parser.add_argument(
        "--morph", default="./build/morph", help="Path to morph binary"
    )
    parser.add_argument(
        "--config", default=None, help="Path to config.toml"
    )
    parser.add_argument(
        "--repeat", type=int, default=1, help="Run each case N times"
    )
    parser.add_argument(
        "--timeout", type=int, default=300, help="Timeout per case (seconds)"
    )
    parser.add_argument(
        "--judge", action="store_true", help="Enable LLM-as-Judge (L3)"
    )
    parser.add_argument(
        "--judge-model", default="gpt-4o-mini", help="Judge LLM model"
    )
    parser.add_argument(
        "--output", default=None, help="Write JSON results to file"
    )
    parser.add_argument(
        "--home", default=None, help="Override HOME for morph subprocess"
    )
    parser.add_argument(
        "--max-workers", type=int, default=1, help="Parallel workers"
    )
    args = parser.parse_args()

    cases = load_cases(args.cases)
    level = args.level
    if not level:
        fname = Path(args.cases).name
        if "tool_selection" in fname:
            level = "tool_selection"
        elif "task_completion" in fname:
            level = "task_completion"
        else:
            print(f"Cannot auto-detect level from {fname}, use --level")
            sys.exit(1)

    adapter = MorphAdapter(
        morph_bin=args.morph,
        config_path=args.config,
        timeout=args.timeout,
        home_dir=args.home,
    )

    print(f"\n{'='*60}")
    print(f"  morph eval — {level}")
    print(f"  cases: {len(cases)} | repeat: {args.repeat}")
    print(f"  morph: {args.morph}")
    print(f"{'='*60}\n")

    if level == "tool_selection":
        result = run_tool_selection(adapter, cases, repeat=args.repeat)
    else:
        result = run_task_completion(
            adapter, cases,
            repeat=args.repeat,
            judge=args.judge,
            judge_model=args.judge_model,
        )

    print_report(result, level)

    if args.output:
        with open(args.output, "w") as f:
            json.dump(result, f, indent=2, ensure_ascii=False)
        print(f"\nResults written to {args.output}")


if __name__ == "__main__":
    main()
