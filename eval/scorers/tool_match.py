"""L1 scorer: tool selection + argument matching."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from adapters.morph_adapter import EvalResult


@dataclass
class ToolMatchScore:
    case_id: str
    tool_match: bool
    args_match: bool
    expected_tool: str
    actual_tool: str | None
    expected_args_contains: dict[str, Any]
    args_details: dict[str, bool]


def _parse_args(args_str: str) -> dict:
    import json
    try:
        return json.loads(args_str)
    except (json.JSONDecodeError, TypeError):
        return {}


def score_tool_match(
    result: EvalResult,
    expected_tools: list[dict],
) -> list[ToolMatchScore]:
    """Score each expected tool call against actual tool calls.

    For each expected tool spec, find the best matching actual call
    (by tool name) and check if expected args are a subset of actual args.
    """
    scores = []
    for exp in expected_tools:
        exp_name = exp.get("name", "")
        exp_args = exp.get("args_contains", {})

        matched_call = None
        for call in result.tool_calls:
            if call.name == exp_name:
                matched_call = call
                break

        tool_match = matched_call is not None
        args_details: dict[str, bool] = {}
        args_match = True

        if matched_call:
            actual_args = _parse_args(matched_call.args)
            for key, val in exp_args.items():
                if key not in actual_args:
                    args_details[key] = False
                    args_match = False
                elif isinstance(val, str) and val.lower() in str(actual_args[key]).lower():
                    args_details[key] = True
                elif actual_args[key] == val:
                    args_details[key] = True
                else:
                    args_details[key] = False
                    args_match = False
        else:
            args_match = False
            for key in exp_args:
                args_details[key] = False

        scores.append(ToolMatchScore(
            case_id="",
            tool_match=tool_match,
            args_match=args_match,
            expected_tool=exp_name,
            actual_tool=matched_call.name if matched_call else None,
            expected_args_contains=exp_args,
            args_details=args_details,
        ))
    return scores


def aggregate_tool_match(
    all_scores: list[tuple[str, list[ToolMatchScore]]],
) -> dict:
    """Aggregate scores across all cases.

    all_scores: list of (case_id, scores_for_case)
    Returns dict with overall and per-category metrics.
    """
    total = len(all_scores)
    tool_correct = 0
    args_correct = 0
    per_tool: dict[str, dict] = {}

    for case_id, scores in all_scores:
        for s in scores:
            if s.tool_match:
                tool_correct += 1
            if s.args_match:
                args_correct += 1
            if s.expected_tool not in per_tool:
                per_tool[s.expected_tool] = {"total": 0, "tool_ok": 0, "args_ok": 0}
            per_tool[s.expected_tool]["total"] += 1
            if s.tool_match:
                per_tool[s.expected_tool]["tool_ok"] += 1
            if s.args_match:
                per_tool[s.expected_tool]["args_ok"] += 1

    n_expected = sum(len(scores) for _, scores in all_scores)
    return {
        "total_cases": total,
        "total_expected_calls": n_expected,
        "tool_accuracy": tool_correct / n_expected if n_expected else 0,
        "args_accuracy": args_correct / n_expected if n_expected else 0,
        "per_tool": per_tool,
    }
