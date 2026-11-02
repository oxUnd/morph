import { useCallback, useEffect, useReducer, useRef, useState } from "react";
import { api, errorMessage } from "@/lib/api";
import { authHeaders, clearAuth } from "@/lib/auth";
import type { ActionRequest } from "@/lib/interactions";
import { parseSSEData, type SSEFrame, SSEParser } from "@/lib/sse";
import { createInitialStreamState, streamReducer } from "@/lib/stream-reducer";
import type { SessionEvent } from "@/types/events";

const MAX_SEEN_EVENTS = 4096;

export function useSessionStream(sessionId: string) {
  const [state, dispatch] = useReducer(streamReducer, undefined, createInitialStreamState);
  const [actionBusy, setActionBusy] = useState<string | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const seenRef = useRef<Set<number>>(new Set());
  const lastEventIdRef = useRef(0);

  useEffect(() => {
    dispatch({ type: "reset" });
    seenRef.current = new Set();
    lastEventIdRef.current = 0;

    let stopped = false;
    let controller: AbortController | null = null;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    let attempt = 0;

    const scheduleReconnect = () => {
      if (stopped) return;
      const delay = Math.min(8_000, 500 * 2 ** attempt) + Math.random() * 250;
      attempt += 1;
      dispatch({ type: "connection", connection: "reconnecting" });
      reconnectTimer = setTimeout(() => void connect(), delay);
    };

    const handleFrame = (frame: SSEFrame) => {
      if (frame.id > 0) {
        if (seenRef.current.has(frame.id)) return;
        seenRef.current.add(frame.id);
        if (seenRef.current.size > MAX_SEEN_EVENTS) seenRef.current.clear();
        lastEventIdRef.current = Math.max(lastEventIdRef.current, frame.id);
      }

      const data = parseSSEData(frame.data);
      const event: SessionEvent = { id: frame.id, type: frame.event, data };
      dispatch({ type: "event", eventId: frame.id, event });

      if (frame.event === "ready") {
        attempt = 0;
        const resumeFrom = (data as { resume_from?: number } | null)?.resume_from ?? 0;
        if (resumeFrom > 0) lastEventIdRef.current = Math.max(lastEventIdRef.current, resumeFrom);
      }
    };

    const connect = async () => {
      if (stopped) return;
      controller = new AbortController();
      const headers: Record<string, string> = {
        Accept: "text/event-stream",
        "Cache-Control": "no-cache",
        ...authHeaders(),
      };
      if (lastEventIdRef.current > 0) {
        headers["Last-Event-ID"] = String(lastEventIdRef.current);
      }

      try {
        const response = await fetch(`/api/sessions/${encodeURIComponent(sessionId)}/events`, {
          headers,
          signal: controller.signal,
          cache: "no-store",
        });
        if (response.status === 401) {
          clearAuth();
          return;
        }
        if (!response.ok || !response.body) {
          throw new Error(`SSE connection failed (${response.status})`);
        }

        dispatch({ type: "connection", connection: "live" });
        const reader = response.body.getReader();
        const decoder = new TextDecoder();
        const parser = new SSEParser();

        while (!stopped) {
          const { value, done } = await reader.read();
          if (done) break;
          const chunk = decoder.decode(value, { stream: true });
          for (const frame of parser.push(chunk)) handleFrame(frame);
        }

        const tail = decoder.decode();
        if (tail) {
          for (const frame of parser.push(tail)) handleFrame(frame);
        }
        for (const frame of parser.flush()) handleFrame(frame);
        if (stopped) return;
        scheduleReconnect();
      } catch (error) {
        if (stopped || (error instanceof DOMException && error.name === "AbortError")) return;
        dispatch({ type: "connection", connection: "offline" });
        scheduleReconnect();
      }
    };

    void connect();

    return () => {
      stopped = true;
      controller?.abort();
      if (reconnectTimer) clearTimeout(reconnectTimer);
    };
  }, [sessionId]);

  const sendPrompt = useCallback(
    async (text: string) => {
      const prompt = text.trim();
      if (!prompt) return;
      dispatch({ type: "user", text: prompt });
      try {
        await api.sendTurn(sessionId, prompt);
      } catch (error) {
        dispatch({ type: "send_error", message: errorMessage(error) });
      }
    },
    [sessionId],
  );

  const respond = useCallback(
    async (interactionId: string, action: ActionRequest) => {
      setActionBusy(interactionId);
      setActionError(null);
      try {
        await api.postAction(sessionId, action.type, action.payload);
        dispatch({ type: "resolve", id: interactionId });
      } catch (error) {
        setActionError(errorMessage(error));
        throw error;
      } finally {
        setActionBusy(null);
      }
    },
    [sessionId],
  );

  return {
    state,
    sendPrompt,
    respond,
    actionBusy,
    actionError,
    clearActionError: () => setActionError(null),
  };
}
