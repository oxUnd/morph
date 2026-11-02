import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";
import { AskUserCard } from "@/components/chat/question-card";
import type { PendingInteraction } from "@/lib/stream-reducer";

const interaction: Extract<PendingInteraction, { kind: "ask_user" }> = {
  kind: "ask_user",
  id: "req-1",
  turnId: "turn-1",
  receivedAt: Date.now(),
  data: {
    request_id: "req-1",
    question: "Deploy to **staging**?",
    choices: ["Staging", "Production"],
    selection_mode: "single",
    min_choices: 1,
    max_choices: 1,
    recommended_indices: [0],
    response_policy: "required",
  },
};

describe("AskUserCard", () => {
  it("renders choices and posts the selected answer action", async () => {
    const user = userEvent.setup();
    const onSubmit = vi.fn().mockResolvedValue(undefined);

    render(<AskUserCard busy={false} interaction={interaction} onSubmit={onSubmit} />);

    expect(screen.getByText("Input requested")).toBeTruthy();
    await user.click(screen.getByRole("button", { name: /Staging/ }));
    await user.click(screen.getByRole("button", { name: /Send answer/ }));

    expect(onSubmit).toHaveBeenCalledWith({
      type: "answer",
      payload: { request_id: "req-1", answers: ["Staging"] },
    });
  });
});
