import { Infinity as InfinityIcon, Loader2, ShieldAlert, ShieldCheck, X } from "lucide-react";
import { useState } from "react";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { CodeBlock } from "@/components/ui/code-block";
import { errorMessage } from "@/lib/api";
import { type ActionRequest, type ApprovalDecision, buildApprovalAction } from "@/lib/interactions";
import type { PendingInteraction } from "@/lib/stream-reducer";
import { formatMaybeJson } from "@/lib/utils";

type ApprovalInteraction = Extract<PendingInteraction, { kind: "approval" }>;

function Field({ label, value }: { label: string; value: string }) {
  if (!value) return null;
  return (
    <div className="min-w-0">
      <dt className="text-[10px] font-medium uppercase tracking-[0.12em] text-muted-foreground">
        {label}
      </dt>
      <dd className="mt-0.5 break-words text-xs text-foreground/90">{value}</dd>
    </div>
  );
}

export function ApprovalCard({
  interaction,
  busy,
  onSubmit,
}: {
  interaction: ApprovalInteraction;
  busy: boolean;
  onSubmit: (action: ActionRequest) => Promise<void>;
}) {
  const [error, setError] = useState<string | null>(null);
  const operationData = interaction.variant === "operation" ? interaction.data : null;
  const toolData = interaction.variant === "tool" ? interaction.data : null;
  const isOperation = operationData !== null;

  async function decide(decision: ApprovalDecision) {
    if (busy) return;
    setError(null);
    try {
      await onSubmit(buildApprovalAction(decision));
    } catch (submitError) {
      setError(errorMessage(submitError));
    }
  }

  const argsText = toolData ? formatMaybeJson(toolData.args) : "";

  return (
    <div className="relative overflow-hidden rounded-2xl border border-amber-500/30 bg-card shadow-[0_12px_36px_-24px_oklch(0.65_0.16_80/0.45)]">
      <div className="absolute inset-x-0 top-0 h-0.5 bg-[linear-gradient(90deg,oklch(0.75_0.16_80),oklch(0.7_0.16_50),oklch(0.75_0.16_80))]" />
      <div className="p-4 sm:p-5">
        <div className="flex items-start gap-3">
          <span className="flex size-8 shrink-0 items-center justify-center rounded-lg bg-amber-500/12 text-amber-700 dark:text-amber-300">
            <ShieldAlert className="size-4" />
          </span>
          <div className="min-w-0 flex-1">
            <div className="flex flex-wrap items-center gap-2">
              <span className="text-[11px] font-semibold uppercase tracking-[0.14em] text-amber-700 dark:text-amber-300">
                {isOperation ? "Operation approval" : "Tool approval"}
              </span>
              <Badge variant="warning">
                {isOperation ? "System access" : "Permission required"}
              </Badge>
            </div>
            <h3 className="mt-1.5 text-[15px] font-semibold tracking-tight">
              Allow <span className="font-mono text-[13px]">{interaction.data.tool}</span>?
            </h3>
            <p className="mt-1 text-xs leading-5 text-muted-foreground">
              {interaction.message || "The agent is waiting for your decision before continuing."}
            </p>
          </div>
        </div>

        {operationData ? (
          <div className="mt-4 space-y-3">
            <dl className="grid gap-3 rounded-xl border border-border/60 bg-background/55 p-3 sm:grid-cols-2">
              <Field label="Action" value={operationData.action} />
              <Field label="Scope" value={operationData.scope} />
              <Field label="Target" value={operationData.target} />
              <Field label="Access" value={operationData.access ?? ""} />
            </dl>
            {operationData.reason && (
              <div className="rounded-lg border border-amber-500/20 bg-amber-500/[0.04] px-3 py-2 text-xs leading-5 text-muted-foreground">
                <span className="font-medium text-foreground">Why: </span>
                {operationData.reason}
              </div>
            )}
            {operationData.programs && operationData.programs.length > 0 && (
              <div className="flex flex-wrap items-center gap-1.5">
                <span className="text-[10px] font-medium uppercase tracking-[0.12em] text-muted-foreground">
                  Programs
                </span>
                {operationData.programs.map((program) => (
                  <Badge key={program} variant="outline">
                    <span className="font-mono">{program}</span>
                  </Badge>
                ))}
              </div>
            )}
            {operationData.target && <CodeBlock label="Target" value={operationData.target} />}
          </div>
        ) : (
          argsText &&
          argsText !== "{}" && (
            <div className="mt-4">
              <CodeBlock label="Arguments" value={argsText} />
            </div>
          )
        )}

        {error && (
          <div className="mt-3 rounded-lg border border-destructive/25 bg-destructive/5 px-2.5 py-1.5 text-[11px] text-destructive">
            {error}
          </div>
        )}

        <div className="mt-4 flex flex-wrap items-center gap-2">
          <span className="text-[10px] text-muted-foreground">This blocks the current turn.</span>
          <div className="ml-auto flex flex-wrap items-center justify-end gap-2">
            <Button
              disabled={busy}
              size="sm"
              type="button"
              variant="outline"
              onClick={() => void decide("reject")}
            >
              {busy ? <Loader2 className="animate-spin" /> : <X />}
              Reject
            </Button>
            <Button
              disabled={busy}
              size="sm"
              type="button"
              variant="secondary"
              onClick={() => void decide("always")}
            >
              <InfinityIcon />
              Always allow
            </Button>
            <Button disabled={busy} size="sm" type="button" onClick={() => void decide("approve")}>
              {busy ? <Loader2 className="animate-spin" /> : <ShieldCheck />}
              Approve once
            </Button>
          </div>
        </div>
      </div>
    </div>
  );
}
