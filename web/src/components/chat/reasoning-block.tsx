import * as Collapsible from "@radix-ui/react-collapsible";
import { Brain, ChevronDown, Loader2 } from "lucide-react";
import { useState } from "react";
import { cn } from "@/lib/utils";

export function ReasoningBlock({
  text,
  source,
  streaming,
}: {
  text: string;
  source: "thought" | "reasoning";
  streaming: boolean;
}) {
  const [open, setOpen] = useState(true);

  return (
    <div className="rounded-xl border border-violet-500/15 bg-violet-500/[0.035]">
      <Collapsible.Root open={open} onOpenChange={setOpen}>
        <Collapsible.Trigger asChild>
          <button
            className="flex w-full items-center gap-2 rounded-xl px-3 py-2 text-left text-[11px] font-medium text-violet-700 outline-none transition-colors hover:bg-violet-500/[0.06] focus-visible:ring-2 focus-visible:ring-ring/60 dark:text-violet-300"
            type="button"
          >
            <Brain className="size-3.5 shrink-0" />
            <span>{source === "thought" ? "Thinking" : "Reasoning"}</span>
            {streaming && (
              <span className="inline-flex items-center gap-1 text-[10px] font-normal text-violet-600/75 dark:text-violet-300/70">
                <Loader2 className="size-3 animate-spin" />
                streaming
              </span>
            )}
            <ChevronDown
              className={cn(
                "ml-auto size-3.5 text-violet-600/60 transition-transform dark:text-violet-300/60",
                open && "rotate-180",
              )}
            />
          </button>
        </Collapsible.Trigger>
        <Collapsible.Content>
          <div className="border-t border-violet-500/10 px-3 py-2.5">
            <div className="whitespace-pre-wrap break-words text-[12px] leading-[1.7] text-muted-foreground">
              {text}
            </div>
          </div>
        </Collapsible.Content>
      </Collapsible.Root>
    </div>
  );
}
