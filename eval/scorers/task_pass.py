"""L2 scorer: task completion with Pass^k metric."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from adapters.morph_adapter import EvalResult


@dataclass
class TaskPassScore:
    case_id: str
    passed: bool
    state_ok: bool
    answer_ok: bool
    tools_sequence_ok: bool
    details: dict[str, Any]


def _check_answer_contains(final_answer: str | None, fragments: list[str]) -> bool:
    if not final_answer or not fragments:
        return not fragments
    answer_lower = final_answer.lower()
    return all(f.lower() in answer_lower for f in fragments)


def _check_tools_sequence(
    actual_calls: list, expected_sequence: list[str]
) -> bool:
    if not expected_sequence:
        return True
    actual_names = [c.name for c in actual_calls]
    if len(actual_names) < len(expected_sequence):
        return False
    for i, exp in enumerate(expected_sequence):
        if actual_names[i] != exp:
            return False
    return True


def score_task_pass(
    result: EvalResult,
    expected_tools_sequence: list[str] | None = None,
    expected_answer_contains: list[str] | None = None,
) -> TaskPassScore:
    """Score a single task completion result."""
    state_ok = result.state == "done"
    answer_ok = _check_answer_contains(
        result.final_answer, expected_answer_contains or []
    )
    tools_ok = _check_tools_sequence(
        result.tool_calls, expected_tools_sequence or []
    )

    passed = state_ok and answer_ok and tools_ok

    return TaskPassScore(
        case_id="",
        passed=passed,
        state_ok=state_ok,
        answer_ok=answer_ok,
        tools_sequence_ok=tools_ok,
        details={
            "state": result.state,
            "final_answer_preview": (result.final_answer or "")[:200],
            "actual_tools": [c.name for c in result.tool_calls],
            "expected_tools": expected_tools_sequence,
            "expected_answer_fragments": expected_answer_contains,
        },
    )


def pass_at_k(results_per_case: dict[str, list[bool]], k: int) -> dict[str, float]:
    """Compute Pass^k: a case passes at k only if all k runs succeed.

    results_per_case: {case_id: [True/False for each run]}
    Returns {case_id: 1.0 or 0.0} and overall rate.
    """
    case_rates = {}
    for case_id, runs in results_per_case.items():
        first_k = runs[:k]
        case_rates[case_id] = 1.0 if all(first_k) else 0.0
    overall = sum(case_rates.values()) / len(case_rates) if case_rates else 0.0
    case_rates["_overall"] = overall
    return case_rates


def aggregate_task_pass(
    scores: list[tuple[str, TaskPassScore]],
) -> dict:
    """Aggregate task pass scores."""
    total = len(scores)
    passed = sum(1 for _, s in scores if s.passed)
    state_ok = sum(1 for _, s in scores if s.state_ok)
    answer_ok = sum(1 for _, s in scores if s.answer_ok)
    tools_ok = sum(1 for _, s in scores if s.tools_sequence_ok)

    per_category: dict[str, dict] = {}
    for case_id, s in scores:
        cat = s.details.get("category", "unknown")
        if cat not in per_category:
            per_category[cat] = {"total": 0, "passed": 0}
        per_category[cat]["total"] += 1
        if s.passed:
            per_category[cat]["passed"] += 1

    return {
        "total_cases": total,
        "passed": passed,
        "pass_rate": passed / total if total else 0,
        "state_ok_rate": state_ok / total if total else 0,
        "answer_ok_rate": answer_ok / total if total else 0,
        "tools_ok_rate": tools_ok / total if total else 0,
        "per_category": per_category,
    }
