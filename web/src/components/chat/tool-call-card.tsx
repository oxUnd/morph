import * as Collapsible from "@radix-ui/react-collapsible";
import { CheckCircle2, ChevronDown, Loader2, SquareTerminal, Wrench } from "lucide-react";
import { useEffect, useState } from "react";
import { Badge } from "@/components/ui/badge";
import { CodeBlock } from "@/components/ui/code-block";
import type { StreamItem } from "@/lib/stream-reducer";
import { cn, formatMaybeJson } from "@/lib/utils";

type ToolItem = Extract<StreamItem, { kind: "tool" }>;

export function ToolCallCard({ item }: { item: ToolItem }) {
  const [open, setOpen] = useState(item.status === "running");
  useEffect(() => {
    if (item.status === "running") setOpen(true);
  }, [item.status]);

  const args = formatMaybeJson(item.args);
  const result = item.result === undefined ? "" : formatMaybeJson(item.result);
  const hasArgs = args && args !== "{}" && args !== "[]";

  return (
    <div className="rounded-xl border border-border/70 bg-card/55">
      <Collapsible.Root open={open} onOpenChange={setOpen}>
        <Collapsible.Trigger asChild>
          <button
            className="flex w-full items-center gap-2 rounded-xl px-3 py-2 text-left outline-none transition-colors hover:bg-muted/45 focus-visible:ring-2 focus-visible:ring-ring/60"
            type="button"
          >
            <span className="flex size-6 shrink-0 items-center justify-center rounded-md bg-muted text-muted-foreground">
              <Wrench className="size-3.5" />
            </span>
            <span className="min-w-0 flex-1">
              <span className="block truncate font-mono text-[11px] font-medium">{item.tool}</span>
              <span className="block text-[10px] text-muted-foreground">
                {item.status === "done" ? "Completed" : "Running"}
              </span>
            </span>
            <Badge variant={item.status === "done" ? "success" : "warning"}>
              {item.status === "done" ? (
                <CheckCircle2 className="size-3" />
              ) : (
                <Loader2 className="size-3 animate-spin" />
              )}
              {item.status === "done" ? "Done" : "Live"}
            </Badge>
            <ChevronDown
              className={cn(
                "size-3.5 text-muted-foreground transition-transform",
                open && "rotate-180",
              )}
            />
          </button>
        </Collapsible.Trigger>
        <Collapsible.Content>
          <div className="space-y-2.5 border-t border-border/60 px-3 py-3">
            {hasArgs && <CodeBlock label="Arguments" value={args} />}
            {item.stream && <CodeBlock label="Stream" value={item.stream} />}
            {result && <CodeBlock label="Result" value={result} />}
            {!hasArgs && !result && !item.stream && (
              <div className="flex items-center gap-2 text-[11px] text-muted-foreground">
                <SquareTerminal className="size-3.5" />
                Waiting for tool output…
              </div>
            )}
          </div>
        </Collapsible.Content>
      </Collapsible.Root>
    </div>
  );
}
