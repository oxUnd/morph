import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";
import { ApprovalCard } from "@/components/chat/approval-card";
import type { PendingInteraction } from "@/lib/stream-reducer";

function approval(
  variant: "tool" | "operation",
): Extract<PendingInteraction, { kind: "approval" }> {
  if (variant === "tool") {
    return {
      kind: "approval",
      id: "turn-1:approval:exec",
      turnId: "turn-1",
      receivedAt: Date.now(),
      variant: "tool",
      message: "Allow exec?",
      data: { tool: "exec", args: { command: "git status" } },
    };
  }
  return {
    kind: "approval",
    id: "turn-1:operation:exec",
    turnId: "turn-1",
    receivedAt: Date.now(),
    variant: "operation",
    message: "Allow git access?",
    data: {
      tool: "exec",
      action: "git push",
      target: "origin/main",
      scope: "session",
      reason: "Push the branch",
      access: "write",
      programs: ["git"],
    },
  };
}

describe("ApprovalCard", () => {
  it("approves a tool once", async () => {
    const user = userEvent.setup();
    const onSubmit = vi.fn().mockResolvedValue(undefined);
    render(<ApprovalCard busy={false} interaction={approval("tool")} onSubmit={onSubmit} />);

    expect(screen.getByText("Tool approval")).toBeTruthy();
    await user.click(screen.getByRole("button", { name: /Approve once/ }));
    expect(onSubmit).toHaveBeenCalledWith({ type: "approve", payload: { always: false } });
  });

  it("allows an operation for the session", async () => {
    const user = userEvent.setup();
    const onSubmit = vi.fn().mockResolvedValue(undefined);
    render(<ApprovalCard busy={false} interaction={approval("operation")} onSubmit={onSubmit} />);

    expect(screen.getByText("Operation approval")).toBeTruthy();
    await user.click(screen.getByRole("button", { name: /Always allow/ }));
    expect(onSubmit).toHaveBeenCalledWith({ type: "always", payload: {} });
  });

  it("rejects an approval", async () => {
    const user = userEvent.setup();
    const onSubmit = vi.fn().mockResolvedValue(undefined);
    render(<ApprovalCard busy={false} interaction={approval("tool")} onSubmit={onSubmit} />);

    await user.click(screen.getByRole("button", { name: /Reject/ }));
    expect(onSubmit).toHaveBeenCalledWith({ type: "reject", payload: {} });
  });
});
