import type { AskUserData } from "@/types/api";

export type ApprovalDecision = "approve" | "reject" | "always";

export interface ActionRequest {
  type: "answer" | "approve" | "reject" | "always";
  payload: Record<string, unknown>;
}

function unique(values: string[]): string[] {
  const seen = new Set<string>();
  const result: string[] = [];
  for (const value of values) {
    const trimmed = value.trim();
    if (!trimmed || seen.has(trimmed)) continue;
    seen.add(trimmed);
    result.push(trimmed);
  }
  return result;
}

export function selectedChoices(data: AskUserData, selected: string[], custom: string): string[] {
  const selectedValues = unique(selected);
  const customValue = custom.trim();
  if (!customValue) return selectedValues;

  if (data.selection_mode === "multi") {
    const max = data.max_choices > 0 ? data.max_choices : Number.POSITIVE_INFINITY;
    return unique([...selectedValues, customValue]).slice(0, max);
  }
  return [customValue];
}

export function toggleChoice(data: AskUserData, selected: string[], choice: string): string[] {
  if (data.selection_mode !== "multi") return [choice];
  if (selected.includes(choice)) return selected.filter((value) => value !== choice);
  if (data.max_choices > 0 && selected.length >= data.max_choices) return selected;
  return [...selected, choice];
}

export function validateAskUser(
  data: AskUserData,
  selected: string[],
  custom: string,
): { valid: boolean; reason: string | null; answers: string[] } {
  const answers = selectedChoices(data, selected, custom);
  const min = Number.isFinite(data.min_choices) ? data.min_choices : 0;
  const max = Number.isFinite(data.max_choices) ? data.max_choices : 0;
  if (answers.length < min) {
    return {
      valid: false,
      reason: min === 1 ? "Choose one answer." : `Choose at least ${min} answers.`,
      answers,
    };
  }
  if (max > 0 && answers.length > max) {
    return { valid: false, reason: `Choose no more than ${max} answers.`, answers };
  }
  if (data.selection_mode !== "multi" && answers.length > 1) {
    return { valid: false, reason: "Choose only one answer.", answers };
  }
  return { valid: true, reason: null, answers };
}

export function buildAskUserAction(
  data: AskUserData,
  selected: string[],
  custom: string,
): ActionRequest {
  return {
    type: "answer",
    payload: {
      request_id: data.request_id,
      answers: selectedChoices(data, selected, custom),
    },
  };
}

export function buildApprovalAction(decision: ApprovalDecision): ActionRequest {
  if (decision === "always") return { type: "always", payload: {} };
  if (decision === "approve") return { type: "approve", payload: { always: false } };
  return { type: "reject", payload: {} };
}
