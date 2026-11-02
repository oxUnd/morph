import type {
  ArtifactData,
  AskUserData,
  OperationApprovalData,
  ToolApprovalData,
} from "@/types/api";
import type {
  ApprovalEventData,
  AskUserEventData,
  OperationApprovalEventData,
  SessionEvent,
  TextEventData,
  ToolCallEventData,
  ToolResultEventData,
  ToolStreamEventData,
  TurnEndEventData,
  TurnStartEventData,
} from "@/types/events";

export type StreamPhase = "idle" | "thinking" | "working" | "waiting" | "done";
export type ConnectionState = "connecting" | "live" | "reconnecting" | "offline";

export type StreamItem =
  | { kind: "user"; id: string; text: string; turnId: string | null }
  | {
      kind: "reasoning";
      id: string;
      turnId: string;
      text: string;
      source: "thought" | "reasoning";
      open: boolean;
    }
  | {
      kind: "tool";
      id: string;
      turnId: string;
      tool: string;
      toolCallId: string;
      args: unknown;
      result?: unknown;
      stream?: string;
      streamKind?: string;
      status: "running" | "done" | "error";
    }
  | { kind: "final"; id: string; turnId: string; text: string }
  | { kind: "reflection"; id: string; turnId: string; text: string }
  | { kind: "error"; id: string; turnId: string | null; message: string }
  | { kind: "artifact"; id: string; turnId: string; artifact: ArtifactData };

export type PendingInteraction =
  | {
      kind: "ask_user";
      id: string;
      turnId: string;
      receivedAt: number;
      data: AskUserData;
    }
  | {
      kind: "approval";
      id: string;
      turnId: string;
      receivedAt: number;
      variant: "tool";
      message: string;
      data: ToolApprovalData;
    }
  | {
      kind: "approval";
      id: string;
      turnId: string;
      receivedAt: number;
      variant: "operation";
      message: string;
      data: OperationApprovalData;
    };

export interface StreamState {
  connection: ConnectionState;
  phase: StreamPhase;
  turnId: string | null;
  queuePosition: number | null;
  items: StreamItem[];
  pending: PendingInteraction[];
  lastEventId: number;
}

export type StreamAction =
  | { type: "reset" }
  | { type: "connection"; connection: ConnectionState }
  | { type: "event"; eventId: number; event: SessionEvent }
  | { type: "user"; text: string }
  | { type: "send_error"; message: string }
  | { type: "resolve"; id: string };

let sequence = 0;
function nextId(prefix: string): string {
  sequence += 1;
  return `${prefix}-${Date.now()}-${sequence}`;
}

export function createInitialStreamState(): StreamState {
  return {
    connection: "connecting",
    phase: "idle",
    turnId: null,
    queuePosition: null,
    items: [],
    pending: [],
    lastEventId: 0,
  };
}

function closeReasoning(items: StreamItem[]): StreamItem[] {
  let changed = false;
  const next = items.map((item) => {
    if (item.kind === "reasoning" && item.open) {
      changed = true;
      return { ...item, open: false };
    }
    return item;
  });
  return changed ? next : items;
}

function appendReasoning(
  items: StreamItem[],
  turnId: string,
  text: string,
  source: "thought" | "reasoning",
): StreamItem[] {
  if (!text) return items;
  for (let index = items.length - 1; index >= 0; index -= 1) {
    const item = items[index];
    if (!item) continue;
    if (item.kind === "reasoning" && item.turnId === turnId && item.open) {
      const next = items.slice();
      next[index] = { ...item, text: item.text + text, source };
      return next;
    }
    if (item.kind === "reasoning" || item.kind === "tool") break;
  }
  return [
    ...items,
    { kind: "reasoning", id: nextId("reasoning"), turnId, text, source, open: true },
  ];
}

function findToolIndex(
  items: StreamItem[],
  turnId: string,
  tool: string,
  toolCallId: string,
): number {
  if (toolCallId) {
    for (let index = items.length - 1; index >= 0; index -= 1) {
      const item = items[index];
      if (item?.kind === "tool" && item.toolCallId === toolCallId) return index;
    }
  }
  for (let index = items.length - 1; index >= 0; index -= 1) {
    const item = items[index];
    if (item?.kind === "tool" && item.turnId === turnId && item.tool === tool) return index;
  }
  return -1;
}

function applyToolCall(items: StreamItem[], data: ToolCallEventData): StreamItem[] {
  const next = closeReasoning(items);
  const existing = findToolIndex(next, data.turn_id, data.tool, data.tool_call_id ?? "");
  if (existing >= 0) {
    const item = next[existing];
    if (item?.kind === "tool") {
      const copy = next.slice();
      copy[existing] = { ...item, tool: data.tool, args: data.args, status: "running" };
      return copy;
    }
  }
  return [
    ...next,
    {
      kind: "tool",
      id: nextId("tool"),
      turnId: data.turn_id,
      tool: data.tool,
      toolCallId: data.tool_call_id ?? "",
      args: data.args,
      status: "running",
    },
  ];
}

function applyToolStream(items: StreamItem[], data: ToolStreamEventData): StreamItem[] {
  const next = closeReasoning(items);
  const existing = findToolIndex(next, data.turn_id, data.tool, data.tool_call_id ?? "");
  if (existing >= 0) {
    const item = next[existing];
    if (item?.kind === "tool") {
      const copy = next.slice();
      copy[existing] = {
        ...item,
        stream: `${item.stream ?? ""}${data.text}`,
        streamKind: data.kind ?? item.streamKind,
      };
      return copy;
    }
  }
  return [
    ...next,
    {
      kind: "tool",
      id: nextId("tool"),
      turnId: data.turn_id,
      tool: data.tool,
      toolCallId: data.tool_call_id ?? "",
      args: {},
      stream: data.text,
      streamKind: data.kind,
      status: "running",
    },
  ];
}

function applyToolResult(items: StreamItem[], data: ToolResultEventData): StreamItem[] {
  const next = closeReasoning(items);
  const existing = findToolIndex(next, data.turn_id, data.tool, data.tool_call_id ?? "");
  if (existing >= 0) {
    const item = next[existing];
    if (item?.kind === "tool") {
      const copy = next.slice();
      copy[existing] = { ...item, result: data.result, status: "done" };
      return copy;
    }
  }
  return [
    ...next,
    {
      kind: "tool",
      id: nextId("tool"),
      turnId: data.turn_id,
      tool: data.tool,
      toolCallId: data.tool_call_id ?? "",
      args: {},
      result: data.result,
      status: "done",
    },
  ];
}

function applyAskUser(state: StreamState, data: AskUserEventData): StreamState {
  const requestId = data.data?.request_id;
  if (!requestId) return state;
  if (state.pending.some((item) => item.id === requestId)) return state;
  return {
    ...state,
    phase: "waiting",
    items: closeReasoning(state.items),
    pending: [
      ...state.pending,
      {
        kind: "ask_user",
        id: requestId,
        turnId: data.turn_id,
        receivedAt: Date.now(),
        data: data.data,
      },
    ],
  };
}

function applyApproval(state: StreamState, data: ApprovalEventData): StreamState {
  const id = `${data.turn_id}:approval:${data.data.tool}`;
  if (state.pending.some((item) => item.id === id)) return state;
  return {
    ...state,
    phase: "waiting",
    items: closeReasoning(state.items),
    pending: [
      ...state.pending,
      {
        kind: "approval",
        id,
        turnId: data.turn_id,
        receivedAt: Date.now(),
        variant: "tool",
        message: data.message,
        data: data.data,
      },
    ],
  };
}

function applyOperationApproval(state: StreamState, data: OperationApprovalEventData): StreamState {
  const id = `${data.turn_id}:operation:${data.data.tool}:${data.data.action}:${data.data.target}`;
  if (state.pending.some((item) => item.id === id)) return state;
  return {
    ...state,
    phase: "waiting",
    items: closeReasoning(state.items),
    pending: [
      ...state.pending,
      {
        kind: "approval",
        id,
        turnId: data.turn_id,
        receivedAt: Date.now(),
        variant: "operation",
        message: data.message,
        data: data.data,
      },
    ],
  };
}

function applyEvent(state: StreamState, event: SessionEvent): StreamState {
  switch (event.type) {
    case "ready": {
      const ready = event.data as { resume_from?: number };
      return {
        ...state,
        connection: "live",
        lastEventId: Math.max(state.lastEventId, ready.resume_from ?? 0),
      };
    }
    case "turn_start": {
      const data = event.data as TurnStartEventData;
      const turnId = data.turn_id || state.turnId;
      const items = state.items.map((item, index) =>
        index === state.items.length - 1 && item.kind === "user" && !item.turnId
          ? { ...item, turnId }
          : item,
      );
      return {
        ...state,
        turnId,
        phase: "working",
        queuePosition: data.queue_position ?? null,
        items,
      };
    }
    case "thought":
    case "reasoning": {
      const data = event.data as TextEventData;
      const source = event.type === "thought" ? "thought" : "reasoning";
      return {
        ...state,
        phase: state.phase === "waiting" ? "waiting" : "thinking",
        items: appendReasoning(state.items, data.turn_id, data.text ?? "", source),
      };
    }
    case "tool_call":
      return {
        ...state,
        phase: "working",
        items: applyToolCall(state.items, event.data as ToolCallEventData),
      };
    case "tool_stream":
      return {
        ...state,
        phase: "working",
        items: applyToolStream(state.items, event.data as ToolStreamEventData),
      };
    case "tool_result":
      return {
        ...state,
        phase: state.phase === "waiting" ? "waiting" : "working",
        items: applyToolResult(state.items, event.data as ToolResultEventData),
      };
    case "reflection": {
      const data = event.data as TextEventData;
      return {
        ...state,
        items: [
          ...closeReasoning(state.items),
          { kind: "reflection", id: nextId("reflection"), turnId: data.turn_id, text: data.text },
        ],
      };
    }
    case "final": {
      const data = event.data as TextEventData;
      const exists = state.items.some(
        (item) => item.kind === "final" && item.turnId === data.turn_id,
      );
      if (exists) return state;
      return {
        ...state,
        phase: "done",
        items: [
          ...closeReasoning(state.items),
          { kind: "final", id: nextId("final"), turnId: data.turn_id, text: data.text },
        ],
      };
    }
    case "turn_end": {
      const data = event.data as TurnEndEventData;
      return {
        ...state,
        phase: "idle",
        turnId: null,
        queuePosition: null,
        items: closeReasoning(state.items),
        pending: state.pending.filter((item) => item.turnId !== data.turn_id),
      };
    }
    case "artifact_ready": {
      const artifact = event.data as ArtifactData;
      return {
        ...state,
        items: [
          ...state.items,
          {
            kind: "artifact",
            id: artifact.id || nextId("artifact"),
            turnId: artifact.turn_id,
            artifact,
          },
        ],
      };
    }
    case "error": {
      const data = event.data as { turn_id?: string; message?: string };
      return {
        ...state,
        phase: "idle",
        items: [
          ...state.items,
          {
            kind: "error",
            id: nextId("error"),
            turnId: data.turn_id ?? null,
            message: data.message ?? "Turn failed",
          },
        ],
      };
    }
    case "ask_user":
      return applyAskUser(state, event.data as AskUserEventData);
    case "approval_required":
      return applyApproval(state, event.data as ApprovalEventData);
    case "operation_approval_required":
      return applyOperationApproval(state, event.data as OperationApprovalEventData);
    case "interaction.resolved": {
      const data = event.data as { request_id?: string; turn_id?: string };
      const pending = state.pending.filter((item) => {
        if (data.request_id) return item.id !== data.request_id;
        if (data.turn_id) return item.turnId !== data.turn_id;
        return false;
      });
      return {
        ...state,
        pending,
        phase: pending.length > 0 ? "waiting" : state.phase === "waiting" ? "working" : state.phase,
      };
    }
    default:
      return state;
  }
}

export function streamReducer(state: StreamState, action: StreamAction): StreamState {
  switch (action.type) {
    case "reset":
      return createInitialStreamState();
    case "connection":
      return { ...state, connection: action.connection };
    case "event":
      return applyEvent(
        { ...state, lastEventId: Math.max(state.lastEventId, action.eventId) },
        action.event,
      );
    case "user":
      return {
        ...state,
        phase: "working",
        items: [
          ...state.items,
          { kind: "user", id: nextId("user"), text: action.text, turnId: null },
        ],
      };
    case "send_error":
      return {
        ...state,
        phase: "idle",
        items: [
          ...state.items,
          { kind: "error", id: nextId("error"), turnId: null, message: action.message },
        ],
      };
    case "resolve":
      return { ...state, pending: state.pending.filter((item) => item.id !== action.id) };
    default:
      return state;
  }
}
