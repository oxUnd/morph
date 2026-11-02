import { describe, expect, it } from "vitest";
import {
  buildApprovalAction,
  buildAskUserAction,
  selectedChoices,
  toggleChoice,
  validateAskUser,
} from "@/lib/interactions";
import type { AskUserData } from "@/types/api";

const base: AskUserData = {
  request_id: "req-1",
  question: "Which target?",
  choices: ["staging", "production"],
  selection_mode: "single",
  min_choices: 1,
  max_choices: 1,
  recommended_indices: [0],
  response_policy: "required",
};

describe("toggleChoice", () => {
  it("keeps a single selection", () => {
    expect(toggleChoice(base, ["staging"], "production")).toEqual(["production"]);
  });

  it("toggles multi-select values and respects max_choices", () => {
    const multi = { ...base, selection_mode: "multi", max_choices: 2 };

    expect(toggleChoice(multi, [], "staging")).toEqual(["staging"]);
    expect(toggleChoice(multi, ["staging"], "staging")).toEqual([]);
    expect(toggleChoice(multi, ["staging", "production"], "canary")).toEqual([
      "staging",
      "production",
    ]);
  });
});

describe("validateAskUser", () => {
  it("requires the configured minimum", () => {
    expect(validateAskUser(base, [], "")).toEqual({
      valid: false,
      reason: "Choose one answer.",
      answers: [],
    });
    expect(validateAskUser({ ...base, min_choices: 2 }, ["staging"], "").valid).toBe(false);
  });

  it("allows custom text to replace a single choice", () => {
    expect(validateAskUser(base, ["staging"], " canary ")).toEqual({
      valid: true,
      reason: null,
      answers: ["canary"],
    });
  });

  it("trims, deduplicates and caps multi-select answers", () => {
    const multi = { ...base, selection_mode: "multi", min_choices: 1, max_choices: 2 };

    expect(selectedChoices(multi, ["staging", "staging"], " canary ")).toEqual([
      "staging",
      "canary",
    ]);
    expect(validateAskUser(multi, ["staging", "production"], "canary").valid).toBe(true);
  });
});

describe("action builders", () => {
  it("builds an answer action with the request id", () => {
    expect(buildAskUserAction(base, ["staging"], "")).toEqual({
      type: "answer",
      payload: { request_id: "req-1", answers: ["staging"] },
    });
  });

  it("maps approval decisions to the FastCGI action contract", () => {
    expect(buildApprovalAction("approve")).toEqual({
      type: "approve",
      payload: { always: false },
    });
    expect(buildApprovalAction("always")).toEqual({ type: "always", payload: {} });
    expect(buildApprovalAction("reject")).toEqual({ type: "reject", payload: {} });
  });
});
