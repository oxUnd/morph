import { Outlet } from "@tanstack/react-router";
import { Menu, X } from "lucide-react";
import { useState } from "react";
import { Brand } from "@/components/brand";
import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";
import { SessionSidebar } from "../sessions/session-sidebar";

export function AppShell() {
  const [open, setOpen] = useState(false);

  return (
    <div className="flex h-dvh overflow-hidden bg-background text-foreground">
      <div
        className={cn(
          "fixed inset-y-0 left-0 z-40 shrink-0 transition-transform duration-200 ease-out lg:static lg:translate-x-0",
          open ? "translate-x-0" : "-translate-x-full",
        )}
      >
        <SessionSidebar onNavigate={() => setOpen(false)} />
      </div>
      {open && (
        <button
          aria-label="Close navigation"
          className="fixed inset-0 z-30 bg-black/35 backdrop-blur-[1px] lg:hidden"
          type="button"
          onClick={() => setOpen(false)}
        />
      )}
      <div className="flex min-w-0 flex-1 flex-col">
        <header className="flex h-14 shrink-0 items-center gap-3 border-b border-border/70 px-3 lg:hidden">
          <Button
            aria-label="Open navigation"
            size="icon"
            variant="ghost"
            onClick={() => setOpen(true)}
          >
            <Menu />
          </Button>
          <Brand compact />
          <span className="text-sm font-semibold tracking-tight">Morph</span>
          <span className="ml-auto text-[10px] font-medium uppercase tracking-[0.16em] text-muted-foreground">
            Console
          </span>
          {open && (
            <Button
              aria-label="Close navigation"
              size="icon"
              variant="ghost"
              onClick={() => setOpen(false)}
            >
              <X />
            </Button>
          )}
        </header>
        <main className="min-h-0 flex-1">
          <Outlet />
        </main>
      </div>
    </div>
  );
}
