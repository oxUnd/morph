import { ArrowUpRight, ShieldCheck, Sparkles, Terminal } from "lucide-react";
import { BrandMark } from "@/components/brand";

const suggestions = [
  {
    icon: Terminal,
    title: "Inspect the repository",
    prompt:
      "Read the project structure and summarize the architecture, then suggest the highest-impact next change.",
  },
  {
    icon: ShieldCheck,
    title: "Review a risky change",
    prompt:
      "Review the current branch for correctness, security, compatibility and missing tests. Lead with actionable findings.",
  },
  {
    icon: Sparkles,
    title: "Build something",
    prompt:
      "Help me turn an idea into a concrete implementation plan, then start with the smallest working slice.",
  },
];

export function EmptyState({ onPick }: { onPick: (prompt: string) => void }) {
  return (
    <div className="mx-auto flex w-full max-w-2xl flex-col items-center px-4 py-12 text-center sm:py-20">
      <div className="relative">
        <div className="absolute inset-0 -z-10 scale-150 rounded-full bg-primary/20 blur-3xl" />
        <BrandMark className="size-14 rounded-2xl [&_svg]:size-8" />
      </div>
      <h1 className="mt-6 text-balance text-2xl font-semibold tracking-[-0.03em] sm:text-3xl">
        What should we work on?
      </h1>
      <p className="mt-2 max-w-md text-pretty text-sm leading-6 text-muted-foreground">
        Ask, plan, inspect a repo, or run tools. Reasoning, approvals and artifacts appear here as
        the turn unfolds.
      </p>
      <div className="mt-8 grid w-full gap-2 text-left sm:grid-cols-3">
        {suggestions.map((suggestion) => (
          <button
            key={suggestion.title}
            className="group flex flex-col rounded-xl border border-border/70 bg-card/65 p-3 text-left outline-none transition-[border-color,background-color,transform] hover:-translate-y-0.5 hover:border-primary/30 hover:bg-card focus-visible:ring-2 focus-visible:ring-ring/60"
            type="button"
            onClick={() => onPick(suggestion.prompt)}
          >
            <suggestion.icon className="size-4 text-primary" />
            <span className="mt-2 text-xs font-medium">{suggestion.title}</span>
            <span className="mt-1 line-clamp-2 text-[11px] leading-4 text-muted-foreground">
              {suggestion.prompt}
            </span>
            <ArrowUpRight className="mt-auto size-3.5 self-end text-muted-foreground/45 transition-colors group-hover:text-primary" />
          </button>
        ))}
      </div>
    </div>
  );
}
