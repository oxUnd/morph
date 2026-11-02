import type {
  ActionResponse,
  CreateSessionResponse,
  HealthResponse,
  LoginResponse,
  MessageListResponse,
  SessionListResponse,
} from "@/types/api";
import { clearAuth, getToken } from "./auth";

const API_BASE = (import.meta.env.VITE_API_BASE ?? "").replace(/\/$/, "");

export class ApiError extends Error {
  readonly status: number;
  readonly body: string;

  constructor(status: number, message: string, body = "") {
    super(message);
    this.name = "ApiError";
    this.status = status;
    this.body = body;
  }
}

export function isApiError(error: unknown): error is ApiError {
  return error instanceof ApiError;
}

export function errorMessage(error: unknown): string {
  if (error instanceof ApiError) return error.message;
  if (error instanceof Error) return error.message;
  return String(error);
}

function buildUrl(path: string): string {
  return `${API_BASE}${path}`;
}

async function readResponse(response: Response): Promise<string> {
  const text = await response.text();
  return text;
}

function messageFromBody(body: string, fallback: string): string {
  if (!body) return fallback;
  try {
    const parsed = JSON.parse(body) as { error?: unknown; message?: unknown };
    if (typeof parsed.error === "string" && parsed.error) return parsed.error;
    if (typeof parsed.message === "string" && parsed.message) return parsed.message;
  } catch {
    /* plain text */
  }
  return body.length > 240 ? `${body.slice(0, 240)}…` : body;
}

async function request<T>(
  path: string,
  init: RequestInit = {},
  options: { auth?: boolean } = {},
): Promise<T> {
  const headers = new Headers(init.headers);
  headers.set("Accept", "application/json");
  if (options.auth !== false) {
    const token = getToken();
    if (token) headers.set("Authorization", `Bearer ${token}`);
  }
  if (init.body !== undefined && !headers.has("Content-Type")) {
    headers.set("Content-Type", "application/json");
  }
  const response = await fetch(buildUrl(path), { ...init, headers });
  const body = await readResponse(response);
  if (!response.ok) {
    if (response.status === 401 && options.auth !== false) clearAuth();
    throw new ApiError(response.status, messageFromBody(body, response.statusText), body);
  }
  if (!body) return undefined as T;
  try {
    return JSON.parse(body) as T;
  } catch {
    throw new ApiError(response.status, "Server returned invalid JSON", body);
  }
}

function jsonBody(value: unknown): string {
  return JSON.stringify(value);
}

function base64Utf8(value: string): string {
  const bytes = new TextEncoder().encode(value);
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

export const api = {
  health(): Promise<HealthResponse> {
    return request<HealthResponse>("/api/health", { method: "GET" }, { auth: false });
  },

  async login(username: string, password: string): Promise<LoginResponse> {
    const response = await fetch(buildUrl("/api/login"), {
      method: "POST",
      headers: {
        Authorization: `Basic ${base64Utf8(`${username}:${password}`)}`,
        Accept: "application/json",
      },
    });
    const body = await readResponse(response);
    if (!response.ok) {
      throw new ApiError(response.status, messageFromBody(body, response.statusText), body);
    }
    return JSON.parse(body) as LoginResponse;
  },

  async install(username: string, password: string) {
    return request<{ user_id: string; username: string; role: string }>("/api/install", {
      method: "POST",
      body: jsonBody({ username, password }),
    });
  },

  async signup(username: string, password: string, signupCode?: string) {
    return request<{ user_id: string; username: string; role: string }>("/api/signup", {
      method: "POST",
      body: jsonBody({ username, password, ...(signupCode ? { signup_code: signupCode } : {}) }),
    });
  },

  logout(): Promise<void> {
    return request<void>("/api/logout", { method: "POST" }).catch(() => undefined);
  },

  listSessions(): Promise<SessionListResponse> {
    return request<SessionListResponse>("/api/sessions?limit=100");
  },

  createSession(name?: string): Promise<CreateSessionResponse> {
    return request<CreateSessionResponse>("/api/sessions", {
      method: "POST",
      body: jsonBody({ name: name ?? "New chat", model: "default" }),
    });
  },

  deleteSession(sessionId: string): Promise<void> {
    return request<void>(`/api/sessions/${encodeURIComponent(sessionId)}`, { method: "DELETE" });
  },

  listMessages(sessionId: string, limit = 100): Promise<MessageListResponse> {
    return request<MessageListResponse>(
      `/api/sessions/${encodeURIComponent(sessionId)}/messages?limit=${limit}`,
    );
  },

  sendTurn(
    sessionId: string,
    prompt: string,
  ): Promise<{ turn_id: string; queue_position: number }> {
    return request<{ turn_id: string; queue_position: number }>(
      `/api/sessions/${encodeURIComponent(sessionId)}/turns`,
      { method: "POST", body: jsonBody({ prompt }) },
    );
  },

  postAction(
    sessionId: string,
    type: string,
    payload: Record<string, unknown> = {},
  ): Promise<ActionResponse> {
    return request<ActionResponse>(`/api/sessions/${encodeURIComponent(sessionId)}/actions`, {
      method: "POST",
      body: jsonBody({ type, payload }),
    });
  },

  async fetchArtifact(url: string): Promise<Blob> {
    const headers = new Headers();
    const token = getToken();
    if (token) headers.set("Authorization", `Bearer ${token}`);
    const response = await fetch(buildUrl(url.startsWith("/") ? url : `/${url}`), { headers });
    if (!response.ok) {
      throw new ApiError(response.status, "Could not load artifact");
    }
    return response.blob();
  },
};
