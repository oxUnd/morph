"""Pretty-print eval results."""

from __future__ import annotations

import sys


def _fmt_pct(val: float) -> str:
    return f"{val * 100:.1f}%"


def print_report(result: dict, level: str) -> None:
    try:
        from rich.console import Console
        from rich.table import Table
        _print_rich(result, level)
    except ImportError:
        _print_plain(result, level)


def _print_rich(result: dict, level: str) -> None:
    from rich.console import Console
    from rich.table import Table

    console = Console()

    if level == "tool_selection":
        table = Table(title="Tool Selection Results")
        table.add_column("Metric", style="cyan")
        table.add_column("Value", style="green")
        table.add_row("Total Cases", str(result["total_cases"]))
        table.add_row("Expected Tool Calls", str(result["total_expected_calls"]))
        table.add_row("Tool Accuracy", _fmt_pct(result["tool_accuracy"]))
        table.add_row("Args Accuracy", _fmt_pct(result["args_accuracy"]))
        console.print(table)

        if result.get("per_tool"):
            t2 = Table(title="Per-Tool Breakdown")
            t2.add_column("Tool", style="cyan")
            t2.add_column("Total")
            t2.add_column("Tool Match", style="green")
            t2.add_column("Args Match", style="green")
            for tool, stats in sorted(result["per_tool"].items()):
                ta = _fmt_pct(stats["tool_ok"] / stats["total"]) if stats["total"] else "N/A"
                aa = _fmt_pct(stats["args_ok"] / stats["total"]) if stats["total"] else "N/A"
                t2.add_row(tool, str(stats["total"]), ta, aa)
            console.print(t2)

    elif level == "task_completion":
        table = Table(title="Task Completion Results")
        table.add_column("Metric", style="cyan")
        table.add_column("Value", style="green")
        table.add_row("Total Cases", str(result["total_cases"]))
        table.add_row("Passed", str(result["passed"]))
        table.add_row("Pass Rate", _fmt_pct(result["pass_rate"]))
        table.add_row("State OK Rate", _fmt_pct(result["state_ok_rate"]))
        table.add_row("Answer OK Rate", _fmt_pct(result["answer_ok_rate"]))
        table.add_row("Tools OK Rate", _fmt_pct(result["tools_ok_rate"]))
        for k in [1, 2, 3]:
            key = f"pass_at_{k}"
            if key in result:
                table.add_row(f"Pass@{k}", _fmt_pct(result[key]))
        console.print(table)

        if result.get("per_category"):
            t2 = Table(title="Per-Category Breakdown")
            t2.add_column("Category", style="cyan")
            t2.add_column("Total")
            t2.add_column("Passed", style="green")
            t2.add_column("Rate", style="green")
            for cat, stats in sorted(result["per_category"].items()):
                rate = _fmt_pct(stats["passed"] / stats["total"]) if stats["total"] else "N/A"
                t2.add_row(cat, str(stats["total"]), str(stats["passed"]), rate)
            console.print(t2)

        if result.get("llm_judge"):
            j = result["llm_judge"]
            t3 = Table(title="LLM-as-Judge")
            t3.add_column("Metric", style="cyan")
            t3.add_column("Value", style="green")
            t3.add_row("Valid Cases", str(j.get("valid_cases", 0)))
            t3.add_row("Avg Score", str(j.get("avg_score", 0)))
            t3.add_row("Distribution", str(j.get("score_distribution", {})))
            console.print(t3)


def _print_plain(result: dict, level: str) -> None:
    if level == "tool_selection":
        print(f"Tool Selection Results")
        print(f"  Total Cases:       {result['total_cases']}")
        print(f"  Expected Calls:    {result['total_expected_calls']}")
        print(f"  Tool Accuracy:     {_fmt_pct(result['tool_accuracy'])}")
        print(f"  Args Accuracy:     {_fmt_pct(result['args_accuracy'])}")
        if result.get("per_tool"):
            print(f"\n  Per-Tool Breakdown:")
            for tool, stats in sorted(result["per_tool"].items()):
                ta = _fmt_pct(stats["tool_ok"] / stats["total"]) if stats["total"] else "N/A"
                aa = _fmt_pct(stats["args_ok"] / stats["total"]) if stats["total"] else "N/A"
                print(f"    {tool:20s}  total={stats['total']}  tool={ta}  args={aa}")

    elif level == "task_completion":
        print(f"Task Completion Results")
        print(f"  Total Cases:       {result['total_cases']}")
        print(f"  Passed:            {result['passed']}")
        print(f"  Pass Rate:         {_fmt_pct(result['pass_rate'])}")
        print(f"  State OK:          {_fmt_pct(result['state_ok_rate'])}")
        print(f"  Answer OK:         {_fmt_pct(result['answer_ok_rate'])}")
        print(f"  Tools OK:          {_fmt_pct(result['tools_ok_rate'])}")
        for k in [1, 2, 3]:
            key = f"pass_at_{k}"
            if key in result:
                print(f"  Pass@{k}:           {_fmt_pct(result[key])}")
        if result.get("per_category"):
            print(f"\n  Per-Category:")
            for cat, stats in sorted(result["per_category"].items()):
                rate = _fmt_pct(stats["passed"] / stats["total"]) if stats["total"] else "N/A"
                print(f"    {cat:20s}  {stats['passed']}/{stats['total']}  {rate}")
