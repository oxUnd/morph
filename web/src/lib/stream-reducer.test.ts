import { describe, expect, it } from "vitest";
import { createInitialStreamState, streamReducer } from "@/lib/stream-reducer";
import type { SessionEvent } from "@/types/events";

function dispatchEvent(
  state: ReturnType<typeof createInitialStreamState>,
  event: SessionEvent,
  eventId = event.id,
) {
  return streamReducer(state, { type: "event", eventId, event });
}

describe("streamReducer interactions", () => {
  it("turns ask_user into a pending interaction", () => {
    const state = dispatchEvent(createInitialStreamState(), {
      id: 1,
      type: "ask_user",
      data: {
        turn_id: "turn-1",
        message: "Need input",
        data: {
          request_id: "req-1",
          question: "Proceed?",
          choices: ["yes", "no"],
          selection_mode: "single",
          min_choices: 1,
          max_choices: 1,
          recommended_indices: [0],
          response_policy: "required",
        },
      },
    });

    expect(state.phase).toBe("waiting");
    expect(state.pending[0]).toMatchObject({
      kind: "ask_user",
      id: "req-1",
      turnId: "turn-1",
    });
  });

  it("distinguishes tool and operation approvals", () => {
    const toolState = dispatchEvent(createInitialStreamState(), {
      id: 2,
      type: "approval_required",
      data: {
        turn_id: "turn-1",
        message: "Allow exec?",
        data: { tool: "exec", args: { command: "ls" } },
      },
    });
    expect(toolState.pending[0]).toMatchObject({
      kind: "approval",
      variant: "tool",
      data: { tool: "exec" },
    });

    const operationState = dispatchEvent(toolState, {
      id: 3,
      type: "operation_approval_required",
      data: {
        turn_id: "turn-1",
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
      },
    });
    expect(operationState.pending[1]).toMatchObject({
      kind: "approval",
      variant: "operation",
      data: { action: "git push" },
    });
  });

  it("removes the interaction when the backend resolves it", () => {
    const waiting = dispatchEvent(createInitialStreamState(), {
      id: 4,
      type: "ask_user",
      data: {
        turn_id: "turn-1",
        message: "Need input",
        data: {
          request_id: "req-1",
          question: "Proceed?",
          choices: ["yes", "no"],
          selection_mode: "single",
          min_choices: 1,
          max_choices: 1,
          recommended_indices: [],
          response_policy: "required",
        },
      },
    });
    const resolved = dispatchEvent(waiting, {
      id: 5,
      type: "interaction.resolved",
      data: { request_id: "req-1" },
    });

    expect(resolved.pending).toEqual([]);
    expect(resolved.phase).toBe("working");
  });
});

describe("streamReducer transcript", () => {
  it("merges reasoning deltas and closes them when a tool starts", () => {
    const first = dispatchEvent(createInitialStreamState(), {
      id: 1,
      type: "reasoning",
      data: { turn_id: "turn-1", text: "Looking " },
    });
    const second = dispatchEvent(first, {
      id: 2,
      type: "thought",
      data: { turn_id: "turn-1", text: "at files" },
    });
    const withTool = dispatchEvent(second, {
      id: 3,
      type: "tool_call",
      data: { turn_id: "turn-1", tool: "file_read", args: { path: "a.c" } },
    });

    expect(withTool.items[0]).toMatchObject({
      kind: "reasoning",
      text: "Looking at files",
      open: false,
    });
    expect(withTool.items[1]).toMatchObject({ kind: "tool", tool: "file_read" });
  });
});
