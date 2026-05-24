"""L3 scorer: LLM-as-Judge for output quality."""

from __future__ import annotations

import json
import os
from dataclasses import dataclass

from adapters.morph_adapter import EvalResult


@dataclass
class LLMJudgeScore:
    case_id: str
    score: int
    reason: str


JUDGE_SYSTEM_PROMPT = """\
You are an evaluation judge. Given a user prompt and an AI agent's final answer,
rate the quality of the answer on a scale of 1-5:

1 = Completely wrong or irrelevant
2 = Partially correct but missing key information
3 = Mostly correct with minor issues
4 = Correct and complete
5 = Excellent, exceeds expectations

Respond with JSON only: {"score": <1-5>, "reason": "<brief explanation>"}
"""

JUDGE_USER_TEMPLATE = """\
User prompt: {prompt}

Agent's final answer: {answer}

Rate the quality of this answer (1-5). Respond with JSON only.
"""


def score_llm_judge(
    result: EvalResult,
    model: str = "gpt-4o-mini",
    api_base: str | None = None,
    api_key_env: str = "OPENAI_API_KEY",
) -> LLMJudgeScore:
    """Use an LLM to judge the quality of the agent's answer."""
    try:
        from openai import OpenAI
    except ImportError:
        return LLMJudgeScore(case_id="", score=0, reason="openai package not installed")

    api_key = os.environ.get(api_key_env, "")
    if not api_key:
        return LLMJudgeScore(case_id="", score=0, reason=f"{api_key_env} not set")

    client = OpenAI(api_key=api_key, base_url=api_base)

    answer = result.final_answer or "(no answer)"
    user_msg = JUDGE_USER_TEMPLATE.format(prompt=result.prompt, answer=answer)

    try:
        resp = client.chat.completions.create(
            model=model,
            messages=[
                {"role": "system", "content": JUDGE_SYSTEM_PROMPT},
                {"role": "user", "content": user_msg},
            ],
            temperature=0,
            max_tokens=256,
        )
        text = resp.choices[0].message.content.strip()
        parsed = json.loads(text)
        return LLMJudgeScore(
            case_id="",
            score=int(parsed.get("score", 0)),
            reason=parsed.get("reason", ""),
        )
    except Exception as exc:
        return LLMJudgeScore(case_id="", score=0, reason=str(exc))


def aggregate_llm_judge(scores: list[tuple[str, LLMJudgeScore]]) -> dict:
    total = len(scores)
    valid = [(cid, s) for cid, s in scores if s.score > 0]
    if not valid:
        return {"total_cases": total, "avg_score": 0, "score_distribution": {}}
    avg = sum(s.score for _, s in valid) / len(valid)
    dist: dict[int, int] = {}
    for _, s in valid:
        dist[s.score] = dist.get(s.score, 0) + 1
    return {
        "total_cases": total,
        "valid_cases": len(valid),
        "avg_score": round(avg, 2),
        "score_distribution": dist,
    }
