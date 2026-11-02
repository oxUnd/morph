import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { Link, useNavigate, useParams } from "@tanstack/react-router";
import { Check, Loader2, LogOut, MessageSquare, Moon, Plus, Sun, Trash2, X } from "lucide-react";
import { useState } from "react";
import { Brand } from "@/components/brand";
import { Button } from "@/components/ui/button";
import { Skeleton } from "@/components/ui/skeleton";
import { api } from "@/lib/api";
import { clearAuth, useAuth } from "@/lib/auth";
import { queryKeys } from "@/lib/query-keys";
import { useTheme } from "@/lib/theme";
import { cn, formatDateTime } from "@/lib/utils";

function SessionRow({
  id,
  name,
  updatedAt,
  active,
  confirming,
  deleting,
  onAskDelete,
  onCancelDelete,
  onDelete,
  onNavigate,
}: {
  id: string;
  name: string;
  updatedAt: number;
  active: boolean;
  confirming: boolean;
  deleting: boolean;
  onAskDelete: () => void;
  onCancelDelete: () => void;
  onDelete: () => void;
  onNavigate?: () => void;
}) {
  return (
    <div className="group relative">
      <Link
        to="/sessions/$sessionId"
        params={{ sessionId: id }}
        onClick={onNavigate}
        className={cn(
          "flex min-h-[52px] items-start gap-2.5 rounded-lg px-2.5 py-2 pr-9 text-sm outline-none transition-colors focus-visible:ring-2 focus-visible:ring-ring/60",
          active
            ? "bg-sidebar-accent text-sidebar-accent-foreground shadow-[inset_2px_0_0_var(--sidebar-primary)]"
            : "text-sidebar-foreground/80 hover:bg-sidebar-accent/60 hover:text-sidebar-foreground",
        )}
      >
        <MessageSquare
          className={cn(
            "mt-0.5 size-4 shrink-0",
            active ? "text-sidebar-primary" : "text-muted-foreground",
          )}
        />
        <span className="min-w-0 flex-1">
          <span className="block truncate font-medium leading-5">{name || "Untitled chat"}</span>
          <span className="mt-0.5 block truncate text-[10px] leading-4 text-muted-foreground">
            {formatDateTime(updatedAt)}
          </span>
        </span>
      </Link>
      <div className="absolute right-1.5 top-1/2 flex -translate-y-1/2 items-center gap-0.5 opacity-0 transition-opacity focus-within:opacity-100 group-hover:opacity-100">
        {confirming ? (
          <>
            <Button
              aria-label="Confirm delete"
              className="size-6 text-destructive"
              disabled={deleting}
              size="icon-sm"
              variant="ghost"
              onClick={onDelete}
            >
              {deleting ? <Loader2 className="animate-spin" /> : <Check />}
            </Button>
            <Button
              aria-label="Cancel delete"
              className="size-6"
              size="icon-sm"
              variant="ghost"
              onClick={onCancelDelete}
            >
              <X />
            </Button>
          </>
        ) : (
          <Button
            aria-label="Delete chat"
            className="size-6 text-muted-foreground"
            size="icon-sm"
            variant="ghost"
            onClick={onAskDelete}
          >
            <Trash2 />
          </Button>
        )}
      </div>
    </div>
  );
}

export function SessionSidebar({ onNavigate }: { onNavigate?: () => void }) {
  const auth = useAuth();
  const { theme, toggle } = useTheme();
  const queryClient = useQueryClient();
  const navigate = useNavigate();
  const params = useParams({ strict: false }) as { sessionId?: string };
  const [confirmingId, setConfirmingId] = useState<string | null>(null);

  const sessions = useQuery({
    queryKey: queryKeys.sessions,
    queryFn: () => api.listSessions(),
    staleTime: 10_000,
  });

  const create = useMutation({
    mutationFn: () => api.createSession(),
    onSuccess: (data) => {
      void queryClient.invalidateQueries({ queryKey: queryKeys.sessions });
      void navigate({
        to: "/sessions/$sessionId",
        params: { sessionId: data.id },
      });
      onNavigate?.();
    },
  });

  const remove = useMutation({
    mutationFn: (sessionId: string) => api.deleteSession(sessionId),
    onSuccess: (_data, sessionId) => {
      setConfirmingId(null);
      void queryClient.invalidateQueries({ queryKey: queryKeys.sessions });
      if (params.sessionId === sessionId) {
        void navigate({ to: "/" });
      }
    },
  });

  const items = (sessions.data?.items ?? [])
    .slice()
    .sort((a, b) => (b.updated_at ?? 0) - (a.updated_at ?? 0));

  return (
    <aside className="flex h-full w-[288px] flex-col border-r border-sidebar-border bg-sidebar text-sidebar-foreground">
      <div className="flex h-14 shrink-0 items-center justify-between px-4">
        <Brand />
        <Button
          aria-label="New chat"
          disabled={create.isPending}
          size="icon-sm"
          variant="ghost"
          onClick={() => create.mutate()}
        >
          {create.isPending ? <Loader2 className="animate-spin" /> : <Plus />}
        </Button>
      </div>

      <div className="px-3 pb-1 pt-2">
        <Button
          className="w-full justify-start border-sidebar-border bg-background/60 shadow-none hover:bg-background"
          disabled={create.isPending}
          variant="outline"
          onClick={() => create.mutate()}
        >
          <Plus />
          New conversation
        </Button>
      </div>

      <div className="mt-3 flex min-h-0 flex-1 flex-col">
        <div className="px-4 pb-1 text-[10px] font-semibold uppercase tracking-[0.16em] text-muted-foreground">
          Conversations
        </div>
        <nav className="min-h-0 flex-1 overflow-y-auto px-2 pb-3">
          {sessions.isPending ? (
            <div className="space-y-2 px-1 py-1">
              {[0, 1, 2, 3].map((index) => (
                <div key={index} className="flex items-center gap-2 px-2 py-2">
                  <Skeleton className="size-4 rounded" />
                  <div className="flex-1 space-y-1.5">
                    <Skeleton className="h-3 w-[75%]" />
                    <Skeleton className="h-2 w-[40%]" />
                  </div>
                </div>
              ))}
            </div>
          ) : sessions.isError ? (
            <div className="mx-1 rounded-lg border border-destructive/20 bg-destructive/5 p-3 text-xs text-muted-foreground">
              <div className="font-medium text-destructive">Could not load chats</div>
              <button
                className="mt-2 font-medium text-foreground underline-offset-4 hover:underline"
                type="button"
                onClick={() => void sessions.refetch()}
              >
                Retry
              </button>
            </div>
          ) : items.length === 0 ? (
            <div className="mx-1 rounded-lg border border-dashed border-sidebar-border px-3 py-5 text-center text-xs leading-5 text-muted-foreground">
              No conversations yet.
              <br />
              Start one above.
            </div>
          ) : (
            <div className="space-y-0.5">
              {items.map((session) => (
                <SessionRow
                  key={session.id}
                  active={params.sessionId === session.id}
                  confirming={confirmingId === session.id}
                  deleting={remove.isPending && remove.variables === session.id}
                  id={session.id}
                  name={session.name}
                  updatedAt={session.updated_at}
                  onAskDelete={() => setConfirmingId(session.id)}
                  onCancelDelete={() => setConfirmingId(null)}
                  onDelete={() => remove.mutate(session.id)}
                  onNavigate={onNavigate}
                />
              ))}
            </div>
          )}
        </nav>
      </div>

      {create.isError && (
        <div className="mx-3 mb-2 rounded-md bg-destructive/10 px-2 py-1.5 text-[11px] text-destructive">
          {create.error instanceof Error ? create.error.message : "Could not create chat"}
        </div>
      )}
      {remove.isError && (
        <div className="mx-3 mb-2 rounded-md bg-destructive/10 px-2 py-1.5 text-[11px] text-destructive">
          {remove.error instanceof Error ? remove.error.message : "Could not delete chat"}
        </div>
      )}

      <div className="shrink-0 border-t border-sidebar-border p-3">
        <div className="flex items-center gap-2 rounded-lg px-1 py-1">
          <div className="flex size-8 shrink-0 items-center justify-center rounded-full bg-primary/10 text-xs font-semibold text-primary">
            {(auth.username ?? "?").slice(0, 1).toUpperCase()}
          </div>
          <div className="min-w-0 flex-1">
            <div className="truncate text-xs font-medium">{auth.username ?? "Operator"}</div>
            <div className="text-[10px] text-muted-foreground">Signed in</div>
          </div>
          <Button
            aria-label={theme === "dark" ? "Use light theme" : "Use dark theme"}
            size="icon-sm"
            variant="ghost"
            onClick={toggle}
          >
            {theme === "dark" ? <Sun /> : <Moon />}
          </Button>
          <Button
            aria-label="Sign out"
            size="icon-sm"
            variant="ghost"
            onClick={() => {
              void api.logout().finally(() => clearAuth());
            }}
          >
            <LogOut />
          </Button>
        </div>
      </div>
    </aside>
  );
}
