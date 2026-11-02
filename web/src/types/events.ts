import type { AskUserData, OperationApprovalData, ToolApprovalData } from "./api";

export type SessionEventType =
  | "ready"
  | "turn_start"
  | "thought"
  | "reasoning"
  | "reflection"
  | "tool_call"
  | "tool_stream"
  | "tool_result"
  | "final"
  | "turn_end"
  | "error"
  | "artifact_ready"
  | "ask_user"
  | "approval_required"
  | "operation_approval_required"
  | "interaction.resolved"
  | "auth_required"
  | string;

export interface SessionEvent<T = unknown> {
  id: number;
  type: SessionEventType;
  data: T;
}

export interface TextEventData {
  turn_id: string;
  text: string;
}

export interface ToolCallEventData {
  turn_id: string;
  tool: string;
  args: unknown;
  tool_call_id?: string;
}

export interface ToolResultEventData {
  turn_id: string;
  tool: string;
  result: unknown;
  tool_call_id?: string;
}

export interface ToolStreamEventData {
  turn_id: string;
  tool: string;
  kind?: string;
  text: string;
  tool_call_id?: string;
}

export interface TurnStartEventData {
  turn_id: string;
  queue_position?: number;
}

export interface TurnEndEventData {
  turn_id: string;
  status?: string;
}

export interface AskUserEventData {
  turn_id: string;
  message: string;
  data: AskUserData;
}

export interface ApprovalEventData {
  turn_id: string;
  message: string;
  data: ToolApprovalData;
}

export interface OperationApprovalEventData {
  turn_id: string;
  message: string;
  data: OperationApprovalData;
}

export interface ResolvedEventData {
  turn_id?: string;
  request_id?: string;
  reason?: string;
}
