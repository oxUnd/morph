import { Check, Copy, Terminal } from "lucide-react";
import { useState } from "react";
import { BrandMark } from "@/components/brand";
import { Markdown } from "@/components/markdown";
import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";

function CopyMessage({ content }: { content: string }) {
  const [copied, setCopied] = useState(false);
  return (
    <Button
      aria-label="Copy message"
      className="size-6 text-muted-foreground opacity-0 transition-opacity group-hover/message:opacity-100 focus-visible:opacity-100"
      size="icon-sm"
      variant="ghost"
      onClick={async () => {
        try {
          await navigator.clipboard.writeText(content);
          setCopied(true);
          window.setTimeout(() => setCopied(false), 1400);
        } catch {
          setCopied(false);
        }
      }}
    >
      {copied ? <Check className="text-emerald-600" /> : <Copy />}
    </Button>
  );
}

export function ChatMessage({
  author,
  content,
  streaming = false,
}: {
  author: string;
  content: string;
  streaming?: boolean;
}) {
  if (!content) return null;

  if (author === "user") {
    return (
      <div className="flex justify-end">
        <div className="max-w-[88%] whitespace-pre-wrap break-words rounded-2xl rounded-br-md bg-primary px-4 py-2.5 text-[13.5px] leading-6 text-primary-foreground shadow-[0_8px_24px_-18px_oklch(0.5_0.2_280/0.75)] sm:max-w-[78%]">
          {content}
        </div>
      </div>
    );
  }

  if (author === "system") {
    return (
      <div className="flex justify-center">
        <div className="max-w-[80%] rounded-full border border-border/60 bg-muted/45 px-3 py-1 text-center text-[11px] leading-5 text-muted-foreground">
          {content}
        </div>
      </div>
    );
  }

  if (author === "tool") {
    return (
      <div className="flex gap-3">
        <span className="mt-0.5 flex size-7 shrink-0 items-center justify-center rounded-lg border border-border/60 bg-muted text-muted-foreground">
          <Terminal className="size-3.5" />
        </span>
        <pre className="min-w-0 flex-1 overflow-x-auto whitespace-pre-wrap break-words rounded-xl border border-border/60 bg-muted/35 px-3 py-2 font-mono text-[11px] leading-5">
          {content}
        </pre>
      </div>
    );
  }

  return (
    <div
      className={cn("group/message flex gap-3", streaming && "animate-[fade-in_160ms_ease-out]")}
    >
      <BrandMark className="mt-0.5 size-7 shrink-0 rounded-lg [&_svg]:size-4" />
      <div className="min-w-0 flex-1">
        <div className="mb-1 flex h-5 items-center gap-1.5">
          <span className="text-[11px] font-semibold tracking-tight text-foreground/80">Morph</span>
          {streaming && (
            <span className="inline-flex items-center gap-1 text-[10px] font-medium text-primary">
              <span className="size-1.5 animate-pulse rounded-full bg-primary" />
              responding
            </span>
          )}
          <CopyMessage content={content} />
        </div>
        <Markdown content={content} />
      </div>
    </div>
  );
}
