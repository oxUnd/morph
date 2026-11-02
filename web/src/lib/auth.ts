import { useSyncExternalStore } from "react";

const TOKEN_KEY = "morph_token";
const USER_KEY = "morph_username";

export interface AuthState {
  token: string | null;
  username: string | null;
}

type Listener = () => void;
const listeners = new Set<Listener>();

function readAuth(): AuthState {
  if (typeof window === "undefined") return { token: null, username: null };
  return {
    token: window.localStorage.getItem(TOKEN_KEY),
    username: window.localStorage.getItem(USER_KEY),
  };
}

let state: AuthState = readAuth();

function emit(next: AuthState) {
  state = next;
  for (const listener of listeners) listener();
}

export function getAuthSnapshot(): AuthState {
  return state;
}

export function subscribeAuth(listener: Listener): () => void {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

export function setAuth(token: string, username: string) {
  window.localStorage.setItem(TOKEN_KEY, token);
  window.localStorage.setItem(USER_KEY, username);
  emit({ token, username });
}

export function clearAuth() {
  if (typeof window !== "undefined") {
    window.localStorage.removeItem(TOKEN_KEY);
    window.localStorage.removeItem(USER_KEY);
  }
  emit({ token: null, username: null });
}

export function getToken(): string | null {
  return state.token;
}

export function authHeaders(): Record<string, string> {
  return state.token ? { Authorization: `Bearer ${state.token}` } : {};
}

export function useAuth(): AuthState {
  return useSyncExternalStore(subscribeAuth, getAuthSnapshot, getAuthSnapshot);
}
