import { useQuery } from "@tanstack/react-query";
import {
  Download,
  ExternalLink,
  FileArchive,
  Film,
  Image as ImageIcon,
  Loader2,
} from "lucide-react";
import { useEffect, useState } from "react";
import { Button } from "@/components/ui/button";
import { api, errorMessage } from "@/lib/api";
import { queryKeys } from "@/lib/query-keys";
import { formatBytes } from "@/lib/utils";
import type { ArtifactData } from "@/types/api";

export function ArtifactCard({ artifact }: { artifact: ArtifactData }) {
  const [objectUrl, setObjectUrl] = useState<string | null>(null);
  const query = useQuery({
    queryKey: queryKeys.artifact(artifact.id),
    queryFn: () => api.fetchArtifact(artifact.url),
    staleTime: 10 * 60_000,
    retry: 1,
  });

  useEffect(() => {
    if (!query.data) return;
    const url = URL.createObjectURL(query.data);
    setObjectUrl(url);
    return () => URL.revokeObjectURL(url);
  }, [query.data]);

  const Icon =
    artifact.kind === "image" ? ImageIcon : artifact.kind === "video" ? Film : FileArchive;

  return (
    <div className="overflow-hidden rounded-xl border border-border/70 bg-card">
      <div className="flex items-center gap-2 border-b border-border/60 px-3 py-2">
        <span className="flex size-7 items-center justify-center rounded-md bg-primary/10 text-primary">
          <Icon className="size-3.5" />
        </span>
        <div className="min-w-0 flex-1">
          <div className="truncate text-xs font-medium">{artifact.filename}</div>
          <div className="text-[10px] text-muted-foreground">
            {artifact.mime || artifact.kind} · {formatBytes(artifact.size_bytes)}
          </div>
        </div>
        {objectUrl && (
          <div className="flex items-center gap-0.5">
            <Button asChild size="icon-sm" variant="ghost">
              <a aria-label="Open artifact" href={objectUrl} rel="noreferrer" target="_blank">
                <ExternalLink />
              </a>
            </Button>
            <Button asChild size="icon-sm" variant="ghost">
              <a aria-label="Download artifact" download={artifact.filename} href={objectUrl}>
                <Download />
              </a>
            </Button>
          </div>
        )}
      </div>
      <div className="bg-muted/25 p-2">
        {query.isPending && (
          <div className="flex h-40 items-center justify-center gap-2 text-xs text-muted-foreground">
            <Loader2 className="size-4 animate-spin" />
            Loading artifact…
          </div>
        )}
        {query.isError && (
          <div className="flex h-40 items-center justify-center px-4 text-center text-xs text-destructive">
            {errorMessage(query.error)}
          </div>
        )}
        {objectUrl && artifact.kind === "image" && (
          <a href={objectUrl} rel="noreferrer" target="_blank">
            <img
              alt={artifact.filename}
              className="mx-auto max-h-[460px] w-auto max-w-full rounded-md object-contain"
              src={objectUrl}
            />
          </a>
        )}
        {objectUrl && artifact.kind === "video" && (
          // biome-ignore lint/a11y/useMediaCaption: generated video artifacts do not ship captions
          <video
            className="mx-auto max-h-[460px] w-full rounded-md bg-black"
            controls
            preload="metadata"
            src={objectUrl}
          />
        )}
        {objectUrl && artifact.kind !== "image" && artifact.kind !== "video" && (
          <a
            className="flex h-40 items-center justify-center gap-2 text-xs font-medium text-primary underline-offset-4 hover:underline"
            download={artifact.filename}
            href={objectUrl}
          >
            <Download className="size-4" />
            Download file
          </a>
        )}
      </div>
    </div>
  );
}
