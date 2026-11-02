import { Check, Copy } from "lucide-react";
import { useState } from "react";
import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";

export function CodeBlock({
  value,
  label,
  className,
}: {
  value: string;
  label?: string;
  className?: string;
}) {
  const [copied, setCopied] = useState(false);

  async function copy() {
    try {
      await navigator.clipboard.writeText(value);
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1400);
    } catch {
      setCopied(false);
    }
  }

  return (
    <div
      className={cn(
        "group/code relative overflow-hidden rounded-lg border border-border/70 bg-muted/45",
        className,
      )}
    >
      {label && (
        <div className="flex items-center justify-between border-b border-border/60 px-3 py-1.5">
          <span className="text-[10px] font-semibold uppercase tracking-[0.12em] text-muted-foreground">
            {label}
          </span>
        </div>
      )}
      <Button
        aria-label="Copy code"
        className="absolute right-1.5 top-1.5 z-10 size-6 bg-background/80 opacity-0 backdrop-blur transition-opacity group-hover/code:opacity-100"
        size="icon-sm"
        variant="ghost"
        onClick={() => void copy()}
      >
        {copied ? <Check className="text-emerald-600" /> : <Copy />}
      </Button>
      <pre className="max-h-[360px] overflow-auto p-3 pr-9 font-mono text-[11px] leading-[1.65] text-foreground/85">
        <code>{value}</code>
      </pre>
    </div>
  );
}
