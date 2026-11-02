import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { Navigate, useNavigate } from "@tanstack/react-router";
import { ArrowRight, Loader2, Plus, TriangleAlert } from "lucide-react";
import { BrandMark } from "@/components/brand";
import { Button } from "@/components/ui/button";
import { Skeleton } from "@/components/ui/skeleton";
import { api, errorMessage } from "@/lib/api";
import { queryKeys } from "@/lib/query-keys";

export function IndexPage() {
  const queryClient = useQueryClient();
  const navigate = useNavigate();
  const sessions = useQuery({
    queryKey: queryKeys.sessions,
    queryFn: () => api.listSessions(),
    staleTime: 10_000,
  });
  const create = useMutation({
    mutationFn: () => api.createSession(),
    onSuccess: (data) => {
      void queryClient.invalidateQueries({ queryKey: queryKeys.sessions });
      void navigate({ to: "/sessions/$sessionId", params: { sessionId: data.id } });
    },
  });

  const items = (sessions.data?.items ?? [])
    .slice()
    .sort((a, b) => (b.updated_at ?? 0) - (a.updated_at ?? 0));

  if (sessions.isPending) {
    return (
      <div className="flex h-full items-center justify-center p-6">
        <div className="w-full max-w-sm space-y-3">
          <Skeleton className="mx-auto size-14 rounded-2xl" />
          <Skeleton className="mx-auto h-7 w-2/3" />
          <Skeleton className="mx-auto h-4 w-4/5" />
          <Skeleton className="mx-auto mt-5 h-9 w-40" />
        </div>
      </div>
    );
  }

  if (sessions.isError) {
    return (
      <div className="flex h-full items-center justify-center p-6">
        <div className="max-w-sm rounded-2xl border border-destructive/20 bg-card p-5 text-center">
          <TriangleAlert className="mx-auto size-6 text-destructive" />
          <div className="mt-2 text-sm font-semibold">Could not load conversations</div>
          <p className="mt-1 text-xs leading-5 text-muted-foreground">
            {errorMessage(sessions.error)}
          </p>
          <Button
            className="mt-4"
            size="sm"
            variant="outline"
            onClick={() => void sessions.refetch()}
          >
            Retry
          </Button>
        </div>
      </div>
    );
  }

  if (items[0]) {
    return <Navigate params={{ sessionId: items[0].id }} replace to="/sessions/$sessionId" />;
  }

  return (
    <div className="flex h-full items-center justify-center overflow-y-auto p-6">
      <div className="flex max-w-lg flex-col items-center text-center">
        <div className="relative">
          <div className="absolute inset-0 -z-10 scale-150 rounded-full bg-primary/20 blur-3xl" />
          <BrandMark className="size-16 rounded-2xl [&_svg]:size-9" />
        </div>
        <h1 className="mt-6 text-balance text-2xl font-semibold tracking-[-0.03em]">
          Your agent workspace is ready
        </h1>
        <p className="mt-2 text-pretty text-sm leading-6 text-muted-foreground">
          Start a conversation to see reasoning, tool activity, approvals and generated artifacts in
          one live timeline.
        </p>
        <Button
          className="mt-6"
          disabled={create.isPending}
          size="lg"
          onClick={() => create.mutate()}
        >
          {create.isPending ? <Loader2 className="animate-spin" /> : <Plus />}
          New conversation
          <ArrowRight />
        </Button>
        {create.isError && (
          <p className="mt-3 text-xs text-destructive">{errorMessage(create.error)}</p>
        )}
      </div>
    </div>
  );
}
