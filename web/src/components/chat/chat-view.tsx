import { useQuery, useQueryClient } from "@tanstack/react-query";
import { ArrowDown, Loader2, Sparkles, TriangleAlert } from "lucide-react";
import { useEffect, useMemo, useRef, useState } from "react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Skeleton } from "@/components/ui/skeleton";
import { useSessionStream } from "@/hooks/use-session-stream";
import { api, errorMessage } from "@/lib/api";
import type { ActionRequest } from "@/lib/interactions";
import { queryKeys } from "@/lib/query-keys";
import type { PendingInteraction, StreamItem } from "@/lib/stream-reducer";
import { cn } from "@/lib/utils";
import { ApprovalCard } from "./approval-card";
import { ArtifactCard } from "./artifact-card";
import { ChatMessage } from "./chat-message";
import { Composer } from "./composer";
import { EmptyState } from "./empty-state";
import { AskUserCard } from "./question-card";
import { ReasoningBlock } from "./reasoning-block";
import { ToolCallCard } from "./tool-call-card";

const connectionCopy = {
  connecting: "Connecting",
  live: "Live",
  reconnecting: "Reconnecting",
  offline: "Offline",
} as const;

function ReflectionBlock({ text }: { text: string }) {
  return (
    <div className="flex gap-2 rounded-lg border border-dashed border-border/70 bg-muted/20 px-3 py-2">
      <Sparkles className="mt-0.5 size-3.5 shrink-0 text-primary/70" />
      <div className="whitespace-pre-wrap break-words text-[11px] leading-5 text-muted-foreground">
        {text}
      </div>
    </div>
  );
}

function StreamItemView({ item }: { item: StreamItem }) {
  switch (item.kind) {
    case "user":
      return <ChatMessage content={item.text} author="user" />;
    case "reasoning":
      return <ReasoningBlock source={item.source} streaming={item.open} text={item.text} />;
    case "tool":
      return <ToolCallCard item={item} />;
    case "final":
      return <ChatMessage content={item.text} author="assistant" />;
    case "reflection":
      return <ReflectionBlock text={item.text} />;
    case "artifact":
      return <ArtifactCard artifact={item.artifact} />;
    case "error":
      return (
        <div className="flex items-start gap-2.5 rounded-xl border border-destructive/25 bg-destructive/[0.045] px-3 py-2.5 text-xs text-destructive">
          <TriangleAlert className="mt-0.5 size-4 shrink-0" />
          <span className="leading-5">{item.message}</span>
        </div>
      );
  }
}

function PendingInteractionView({
  interaction,
  busy,
  onSubmit,
}: {
  interaction: PendingInteraction;
  busy: boolean;
  onSubmit: (action: ActionRequest) => Promise<void>;
}) {
  if (interaction.kind === "ask_user") {
    return <AskUserCard busy={busy} interaction={interaction} onSubmit={onSubmit} />;
  }
  return <ApprovalCard busy={busy} interaction={interaction} onSubmit={onSubmit} />;
}

export function ChatView({ sessionId, title }: { sessionId: string; title: string }) {
  const { state, sendPrompt, respond, actionBusy, actionError, clearActionError } =
    useSessionStream(sessionId);
  const queryClient = useQueryClient();
  const messagesQuery = useQuery({
    queryKey: queryKeys.messages(sessionId),
    queryFn: () => api.listMessages(sessionId),
    refetchOnMount: "always",
    staleTime: 2_000,
  });

  const history = useMemo(
    () => [...(messagesQuery.data?.items ?? [])].sort((a, b) => a.created_at - b.created_at),
    [messagesQuery.data?.items],
  );
  const liveTurnIds = useMemo(
    () =>
      new Set(
        state.items
          .map((item) => item.turnId)
          .filter((turnId): turnId is string => Boolean(turnId)),
      ),
    [state.items],
  );
  const visibleHistory = useMemo(
    () => history.filter((message) => !message.turn_id || !liveTurnIds.has(message.turn_id)),
    [history, liveTurnIds],
  );

  const scrollRef = useRef<HTMLDivElement>(null);
  const endRef = useRef<HTMLDivElement>(null);
  const stickRef = useRef(true);
  const [stick, setStick] = useState(true);
  const previousTurnRef = useRef<string | null>(null);

  useEffect(() => {
    const hadTurn = Boolean(previousTurnRef.current);
    if (hadTurn && !state.turnId) {
      void queryClient.invalidateQueries({ queryKey: queryKeys.messages(sessionId) });
      void queryClient.invalidateQueries({ queryKey: queryKeys.sessions });
    }
    previousTurnRef.current = state.turnId;
  }, [queryClient, sessionId, state.turnId]);

  // biome-ignore lint/correctness/useExhaustiveDependencies: transcript updates should keep the view pinned
  useEffect(() => {
    if (!stickRef.current) return;
    endRef.current?.scrollIntoView({ block: "end" });
  }, [messagesQuery.isPending, state.items, state.pending]);

  function handleScroll() {
    const element = scrollRef.current;
    if (!element) return;
    const nextStick = element.scrollHeight - element.scrollTop - element.clientHeight < 120;
    stickRef.current = nextStick;
    setStick(nextStick);
  }

  function jumpToLatest() {
    stickRef.current = true;
    setStick(true);
    endRef.current?.scrollIntoView({ behavior: "smooth", block: "end" });
  }

  async function handleSend(text: string) {
    stickRef.current = true;
    setStick(true);
    await sendPrompt(text);
  }

  const waiting = state.pending.length > 0 || state.phase === "waiting";
  const busy = state.turnId !== null || waiting;
  const phaseBadge =
    state.phase === "thinking"
      ? { label: "Thinking", variant: "secondary" as const }
      : state.phase === "working"
        ? { label: "Working", variant: "default" as const }
        : state.phase === "waiting"
          ? { label: "Waiting for you", variant: "warning" as const }
          : state.phase === "done"
            ? { label: "Done", variant: "success" as const }
            : null;
  const showEmpty =
    visibleHistory.length === 0 &&
    state.items.length === 0 &&
    state.pending.length === 0 &&
    !messagesQuery.isPending;
  const connectionTone =
    state.connection === "live"
      ? "bg-emerald-500"
      : state.connection === "connecting" || state.connection === "reconnecting"
        ? "bg-amber-500"
        : "bg-destructive";

  return (
    <div className="flex h-full min-h-0 flex-col">
      <header className="flex h-14 shrink-0 items-center gap-3 border-b border-border/70 bg-background/80 px-4 backdrop-blur sm:px-6">
        <div className="min-w-0 flex-1">
          <h2 className="truncate text-sm font-semibold tracking-tight">
            {title || "Conversation"}
          </h2>
          <div className="flex items-center gap-1.5 text-[10px] text-muted-foreground">
            <span className={cn("size-1.5 rounded-full", connectionTone)} />
            <span>{connectionCopy[state.connection]}</span>
            {state.queuePosition !== null && (
              <>
                <span className="text-border">·</span>
                <span>queued #{state.queuePosition + 1}</span>
              </>
            )}
          </div>
        </div>
        {phaseBadge && (
          <Badge variant={phaseBadge.variant}>
            {(state.phase === "thinking" || state.phase === "working") && (
              <Loader2 className="size-3 animate-spin" />
            )}
            {phaseBadge.label}
          </Badge>
        )}
      </header>

      <div
        ref={scrollRef}
        className="relative min-h-0 flex-1 overflow-y-auto overscroll-contain scroll-pb-6"
        onScroll={handleScroll}
      >
        <div className="mx-auto flex w-full max-w-4xl flex-col gap-5 px-4 py-6 sm:px-6">
          {messagesQuery.isError && (
            <div className="flex items-center gap-2 rounded-lg border border-destructive/25 bg-destructive/5 px-3 py-2 text-[11px] text-destructive">
              <TriangleAlert className="size-3.5 shrink-0" />
              <span className="min-w-0 flex-1">
                Could not load message history: {errorMessage(messagesQuery.error)}
              </span>
              <Button size="sm" variant="ghost" onClick={() => void messagesQuery.refetch()}>
                Retry
              </Button>
            </div>
          )}

          {messagesQuery.isPending && visibleHistory.length === 0 && state.items.length === 0 && (
            <div className="space-y-4 pt-4" role="status" aria-label="Loading conversation">
              <div className="flex justify-end">
                <Skeleton className="h-10 w-1/2 rounded-2xl" />
              </div>
              <div className="flex gap-3">
                <Skeleton className="size-7 shrink-0 rounded-lg" />
                <div className="flex-1 space-y-2">
                  <Skeleton className="h-4 w-1/3" />
                  <Skeleton className="h-24 w-full rounded-xl" />
                </div>
              </div>
            </div>
          )}

          {visibleHistory.map((message) => (
            <ChatMessage key={message.id} content={message.content} author={message.role} />
          ))}

          {state.items.map((item) => (
            <StreamItemView key={item.id} item={item} />
          ))}

          {state.pending.map((interaction) => (
            <PendingInteractionView
              key={interaction.id}
              busy={actionBusy === interaction.id}
              interaction={interaction}
              onSubmit={(action) => respond(interaction.id, action)}
            />
          ))}

          {actionError && (
            <div className="flex items-start gap-2 rounded-lg border border-destructive/25 bg-destructive/5 px-3 py-2 text-[11px] text-destructive">
              <TriangleAlert className="mt-0.5 size-3.5 shrink-0" />
              <span className="min-w-0 flex-1">{actionError}</span>
              <Button size="sm" variant="ghost" onClick={clearActionError}>
                Dismiss
              </Button>
            </div>
          )}

          {showEmpty && <EmptyState onPick={(prompt) => void handleSend(prompt)} />}
          <div ref={endRef} />
        </div>

        {!stick && (
          <div className="pointer-events-none sticky bottom-3 z-10 flex justify-center">
            <Button
              className="pointer-events-auto h-8 rounded-full border-border/80 bg-background/90 px-3 text-[11px] shadow-lg backdrop-blur"
              size="sm"
              variant="outline"
              onClick={jumpToLatest}
            >
              <ArrowDown />
              Jump to latest
            </Button>
          </div>
        )}
      </div>

      <div className="shrink-0 border-t border-border/60 bg-background/85 px-3 py-3 backdrop-blur sm:px-6">
        <div className="mx-auto max-w-4xl">
          <Composer
            disabled={busy}
            placeholder={
              waiting
                ? "Respond to the prompt above to continue…"
                : busy
                  ? "Morph is working…"
                  : "Message Morph…"
            }
            onSend={handleSend}
          />
          <p className="mt-1.5 text-center text-[10px] leading-4 text-muted-foreground">
            Review tool actions before approving. The agent can execute commands when you allow it.
          </p>
        </div>
      </div>
    </div>
  );
}
