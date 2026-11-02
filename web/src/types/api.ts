export interface HealthResponse {
  status: string;
  setup_required: boolean;
  ts: number;
  runtime_pool?: {
    workers: number;
    busy: number;
    starting: number;
    min: number;
    max: number;
    waiting: number;
    queue_max: number;
    idle_seconds: number;
  };
  metrics?: {
    requests_total: number;
    active: number;
    "2xx": number;
    "4xx": number;
    "5xx": number;
  };
}

export interface LoginResponse {
  token: string;
  user_id: string;
  username: string;
  role: string;
  expires_at: number;
}

export interface SessionSummary {
  id: string;
  name: string;
  model: string;
  created_at: number;
  updated_at: number;
}

export interface SessionListResponse {
  items: SessionSummary[];
  limit: number;
  offset: number;
  has_more: boolean;
}

export interface SessionMessage {
  id: string;
  role: string;
  content: string;
  turn_id: string | null;
  token_count: number;
  compressed: number;
  created_at: number;
}

export interface MessageListResponse {
  items: SessionMessage[];
  limit: number;
  has_more: boolean;
}

export interface CreateSessionResponse {
  id: string;
}

export interface ActionResponse {
  queued: boolean;
}

export interface AskUserData {
  request_id: string;
  question: string;
  choices: string[];
  selection_mode: "single" | "multi" | string;
  min_choices: number;
  max_choices: number;
  recommended_indices: number[];
  response_policy?: "optional" | "required";
  timeout_seconds?: number;
}

export interface ToolApprovalData {
  tool: string;
  args: unknown;
}

export interface OperationApprovalData {
  tool: string;
  action: string;
  target: string;
  scope: string;
  reason: string;
  access?: string;
  programs?: string[];
}

export interface ArtifactData {
  id: string;
  kind: "image" | "video" | "file" | string;
  mime: string;
  filename: string;
  size_bytes: number;
  url: string;
  turn_id: string;
}
