import { useQuery } from "@tanstack/react-query";
import { useParams } from "@tanstack/react-router";
import { ChatView } from "@/components/chat/chat-view";
import { api } from "@/lib/api";
import { queryKeys } from "@/lib/query-keys";

export function SessionPage() {
  const { sessionId } = useParams({ from: "/sessions/$sessionId" });
  const sessions = useQuery({
    queryKey: queryKeys.sessions,
    queryFn: () => api.listSessions(),
    staleTime: 10_000,
  });
  const session = sessions.data?.items.find((item) => item.id === sessionId);

  return <ChatView key={sessionId} sessionId={sessionId} title={session?.name ?? "Conversation"} />;
}
