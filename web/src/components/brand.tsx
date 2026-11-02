import { cn } from "@/lib/utils";

export function BrandMark({ className }: { className?: string }) {
  return (
    <span
      className={cn(
        "relative inline-flex size-9 items-center justify-center overflow-hidden rounded-xl bg-[linear-gradient(145deg,oklch(0.62_0.22_275),oklch(0.52_0.2_300))] text-white shadow-[0_6px_20px_-8px_oklch(0.55_0.22_280/0.8)]",
        className,
      )}
      aria-hidden="true"
    >
      <svg viewBox="0 0 32 32" className="size-5" fill="none">
        <title>Morph</title>
        <path
          d="M7 23V10.5c0-1.4 1.7-2.1 2.7-1.1L16 15.7l6.3-6.3c1-1 2.7-.3 2.7 1.1V23"
          stroke="currentColor"
          strokeWidth="2.6"
          strokeLinecap="round"
          strokeLinejoin="round"
        />
        <circle cx="16" cy="23" r="2.2" fill="currentColor" />
      </svg>
      <span className="pointer-events-none absolute inset-x-0 top-0 h-1/2 bg-white/15" />
    </span>
  );
}

export function Brand({ compact = false }: { compact?: boolean }) {
  return (
    <div className="flex min-w-0 items-center gap-2.5">
      <BrandMark />
      {!compact && (
        <div className="min-w-0 leading-tight">
          <div className="truncate text-[15px] font-semibold tracking-tight">Morph</div>
          <div className="truncate text-[10px] font-medium uppercase tracking-[0.18em] text-muted-foreground">
            Console
          </div>
        </div>
      )}
    </div>
  );
}
