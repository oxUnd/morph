import { Check, Clock3, Loader2, MessageCircleQuestion, Send, Sparkles } from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import { Markdown } from "@/components/markdown";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { errorMessage } from "@/lib/api";
import {
  type ActionRequest,
  buildAskUserAction,
  toggleChoice,
  validateAskUser,
} from "@/lib/interactions";
import type { PendingInteraction } from "@/lib/stream-reducer";
import { cn } from "@/lib/utils";

type AskInteraction = Extract<PendingInteraction, { kind: "ask_user" }>;

function countdownLabel(seconds: number): string {
  const minutes = Math.floor(seconds / 60);
  const rest = seconds % 60;
  return `${minutes}:${rest.toString().padStart(2, "0")}`;
}

export function AskUserCard({
  interaction,
  busy,
  onSubmit,
}: {
  interaction: AskInteraction;
  busy: boolean;
  onSubmit: (action: ActionRequest) => Promise<void>;
}) {
  const data = interaction.data;
  const [selected, setSelected] = useState<string[]>([]);
  const [custom, setCustom] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [now, setNow] = useState(() => Date.now());

  const timeoutSeconds = data.timeout_seconds ?? 0;
  const deadline = timeoutSeconds > 0 ? interaction.receivedAt + timeoutSeconds * 1000 : 0;
  const remaining = deadline > 0 ? Math.max(0, Math.ceil((deadline - now) / 1000)) : null;
  const expired = remaining !== null && remaining <= 0;
  const validation = useMemo(
    () => validateAskUser(data, selected, custom),
    [custom, data, selected],
  );

  useEffect(() => {
    if (!deadline) return;
    const interval = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(interval);
  }, [deadline]);

  async function submit(answers?: string[]) {
    if (busy || expired) return;
    if (!answers && !validation.valid) {
      setError(validation.reason);
      return;
    }
    setError(null);
    try {
      await onSubmit(
        answers
          ? { type: "answer", payload: { request_id: data.request_id, answers } }
          : buildAskUserAction(data, selected, custom),
      );
    } catch (submitError) {
      setError(errorMessage(submitError));
    }
  }

  const multiselect = data.selection_mode === "multi";
  const recommended = new Set(data.recommended_indices ?? []);
  const min = Number.isFinite(data.min_choices) ? data.min_choices : 0;
  const max = Number.isFinite(data.max_choices) ? data.max_choices : 0;

  return (
    <div className="relative overflow-hidden rounded-2xl border border-primary/25 bg-card shadow-[0_12px_36px_-24px_oklch(0.5_0.2_280/0.5)]">
      <div className="absolute inset-x-0 top-0 h-0.5 bg-[linear-gradient(90deg,var(--primary),oklch(0.65_0.18_190),var(--primary))] opacity-80" />
      <div className="p-4 sm:p-5">
        <div className="flex items-start gap-3">
          <span className="flex size-8 shrink-0 items-center justify-center rounded-lg bg-primary/10 text-primary">
            <MessageCircleQuestion className="size-4" />
          </span>
          <div className="min-w-0 flex-1">
            <div className="flex flex-wrap items-center gap-2">
              <span className="text-[11px] font-semibold uppercase tracking-[0.14em] text-primary">
                Input requested
              </span>
              <Badge variant={data.response_policy === "optional" ? "outline" : "warning"}>
                {data.response_policy === "optional" ? "Optional" : "Required"}
              </Badge>
              {multiselect && <Badge variant="secondary">Multi-select</Badge>}
            </div>
            <div className="mt-2 text-[15px] leading-6">
              <Markdown content={data.question || "The agent needs your input."} />
            </div>
          </div>
        </div>

        {data.choices.length > 0 && (
          <fieldset
            className={cn(
              "mt-4 grid gap-2",
              data.choices.length > 3 ? "sm:grid-cols-2" : "grid-cols-1",
            )}
            aria-label="Answer choices"
          >
            {data.choices.map((choice, index) => {
              const active = selected.includes(choice);
              const atLimit = multiselect && max > 0 && selected.length >= max && !active;
              return (
                <button
                  key={choice}
                  aria-pressed={active}
                  className={cn(
                    "group flex min-h-11 w-full items-start gap-2.5 rounded-xl border px-3 py-2.5 text-left text-sm outline-none transition-[border-color,background-color,box-shadow] focus-visible:ring-2 focus-visible:ring-ring/60",
                    active
                      ? "border-primary/60 bg-primary/[0.07] shadow-[inset_0_0_0_1px_oklch(0.55_0.2_280/0.15)]"
                      : "border-border/70 bg-background/60 hover:border-primary/35 hover:bg-primary/[0.03]",
                    atLimit && "cursor-not-allowed opacity-55",
                  )}
                  disabled={busy || expired || atLimit}
                  type="button"
                  onClick={() => {
                    setSelected((current) => toggleChoice(data, current, choice));
                    setError(null);
                  }}
                >
                  <span
                    className={cn(
                      "mt-0.5 flex size-4 shrink-0 items-center justify-center border transition-colors",
                      multiselect ? "rounded-[4px]" : "rounded-full",
                      active
                        ? "border-primary bg-primary text-primary-foreground"
                        : "border-muted-foreground/35 bg-background",
                    )}
                  >
                    {active && <Check className="size-3" strokeWidth={3} />}
                  </span>
                  <span className="min-w-0 flex-1 leading-5">{choice}</span>
                  {recommended.has(index) && (
                    <Badge className="mt-0.5 shrink-0" variant="success">
                      <Sparkles className="size-2.5" />
                      Recommended
                    </Badge>
                  )}
                </button>
              );
            })}
          </fieldset>
        )}

        <div className="mt-4">
          <Input
            aria-label="Custom answer"
            disabled={busy || expired}
            placeholder={multiselect ? "Add your own answer…" : "Or type a custom answer…"}
            value={custom}
            onChange={(event) => {
              setCustom(event.target.value);
              setError(null);
            }}
            onKeyDown={(event) => {
              if (event.key === "Enter" && !event.nativeEvent.isComposing) {
                event.preventDefault();
                void submit();
              }
            }}
          />
          <div className="mt-1.5 text-[10px] leading-4 text-muted-foreground">
            {custom.trim()
              ? multiselect
                ? "Your text is added to the selected answers."
                : "Your text replaces the selected choice for a single-select prompt."
              : min > 0 || max > 0
                ? `${min > 1 ? `At least ${min}` : "Choose"}${max > 0 ? `, up to ${max}` : ""} · ${selected.length} selected`
                : "Optional prompt — you can skip it."}
          </div>
        </div>

        {(error || validation.reason) && !expired && (
          <div className="mt-3 rounded-lg border border-destructive/25 bg-destructive/5 px-2.5 py-1.5 text-[11px] text-destructive">
            {error ?? validation.reason}
          </div>
        )}
        {expired && (
          <div className="mt-3 rounded-lg border border-amber-500/25 bg-amber-500/5 px-2.5 py-1.5 text-[11px] text-amber-700 dark:text-amber-300">
            This prompt timed out. The agent is continuing on its own.
          </div>
        )}

        <div className="mt-4 flex flex-wrap items-center gap-2">
          {remaining !== null && !expired && (
            <span
              className={cn(
                "inline-flex items-center gap-1 text-[11px] tabular-nums",
                remaining <= 10 ? "text-destructive" : "text-muted-foreground",
              )}
            >
              <Clock3 className="size-3" />
              {countdownLabel(remaining)} left
            </span>
          )}
          <div className="ml-auto flex items-center gap-2">
            {min === 0 && (
              <Button
                disabled={busy || expired}
                size="sm"
                type="button"
                variant="ghost"
                onClick={() => void submit([])}
              >
                Skip
              </Button>
            )}
            <Button
              disabled={busy || expired || !validation.valid}
              size="sm"
              type="button"
              onClick={() => void submit()}
            >
              {busy ? <Loader2 className="animate-spin" /> : <Send />}
              Send answer
            </Button>
          </div>
        </div>
      </div>
    </div>
  );
}
