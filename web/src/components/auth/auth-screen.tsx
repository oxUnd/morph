import { useQuery } from "@tanstack/react-query";
import {
  ArrowRight,
  CheckCircle2,
  KeyRound,
  Loader2,
  LockKeyhole,
  MessagesSquare,
  ShieldCheck,
  SquareTerminal,
  UserRound,
} from "lucide-react";
import { type FormEvent, useState } from "react";
import { Brand, BrandMark } from "@/components/brand";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card";
import { Input } from "@/components/ui/input";
import { api } from "@/lib/api";
import { setAuth } from "@/lib/auth";

type Mode = "login" | "signup" | "install";

function errorText(error: unknown): string {
  return error instanceof Error ? error.message : "Something went wrong";
}

export function AuthScreen() {
  const health = useQuery({
    queryKey: ["health"],
    queryFn: () => api.health(),
    retry: false,
    staleTime: 15_000,
  });
  const setupRequired = health.data?.setup_required === true;
  const [mode, setMode] = useState<Mode | null>(null);
  const activeMode: Mode = mode ?? (setupRequired ? "install" : "login");
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [confirm, setConfirm] = useState("");
  const [signupCode, setSignupCode] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState(false);

  async function handleSubmit(event: FormEvent) {
    event.preventDefault();
    setError(null);
    if (!username.trim() || !password) {
      setError("Username and password are required.");
      return;
    }
    if ((activeMode === "install" || activeMode === "signup") && password !== confirm) {
      setError("Passwords do not match.");
      return;
    }
    setSubmitting(true);
    try {
      if (activeMode === "install") {
        await api.install(username.trim(), password);
      } else if (activeMode === "signup") {
        await api.signup(username.trim(), password, signupCode.trim() || undefined);
      }
      const session = await api.login(username.trim(), password);
      setAuth(session.token, session.username);
    } catch (submitError) {
      setError(errorText(submitError));
    } finally {
      setSubmitting(false);
    }
  }

  const switchMode = (next: Mode) => {
    setMode(next);
    setError(null);
    setConfirm("");
  };

  return (
    <div className="relative flex min-h-dvh overflow-hidden bg-background">
      <div className="pointer-events-none absolute inset-0 bg-[radial-gradient(circle_at_15%_10%,oklch(0.72_0.16_280/0.12),transparent_35%),radial-gradient(circle_at_85%_85%,oklch(0.7_0.14_190/0.1),transparent_35%)]" />
      <section className="relative hidden w-[46%] min-w-[420px] flex-col justify-between border-r border-border/60 bg-muted/20 p-10 lg:flex xl:p-14">
        <Brand />
        <div className="max-w-md">
          <div className="mb-5 inline-flex items-center gap-2 rounded-full border border-border/70 bg-background/70 px-3 py-1 text-xs text-muted-foreground backdrop-blur">
            <span className="size-1.5 rounded-full bg-emerald-500 shadow-[0_0_8px_oklch(0.72_0.17_160)]" />
            FastCGI runtime console
          </div>
          <h1 className="text-balance text-4xl font-semibold tracking-[-0.04em] xl:text-5xl">
            One place to see what your agent is doing.
          </h1>
          <p className="mt-4 text-pretty text-[15px] leading-6 text-muted-foreground">
            Streaming reasoning, tool execution, operator approvals and interactive prompts —
            rendered as a first-class conversation.
          </p>
          <div className="mt-8 grid gap-3">
            {[
              [
                MessagesSquare,
                "Live transcripts",
                "Thoughts, tools and answers stream in one timeline.",
              ],
              [
                ShieldCheck,
                "Human in the loop",
                "Approve tools, shell operations or grant always-allow.",
              ],
              [
                SquareTerminal,
                "Runtime-native",
                "Built on the existing FastCGI HTTP and SSE contract.",
              ],
            ].map(([Icon, title, description]) => {
              const FeatureIcon = Icon as typeof MessagesSquare;
              return (
                <div
                  key={String(title)}
                  className="flex gap-3 rounded-xl border border-border/60 bg-background/55 p-3.5 backdrop-blur-sm"
                >
                  <span className="flex size-8 shrink-0 items-center justify-center rounded-lg bg-primary/10 text-primary">
                    <FeatureIcon className="size-4" />
                  </span>
                  <div>
                    <div className="text-sm font-medium">{String(title)}</div>
                    <div className="mt-0.5 text-xs leading-5 text-muted-foreground">
                      {String(description)}
                    </div>
                  </div>
                </div>
              );
            })}
          </div>
        </div>
        <div className="flex items-center gap-2 text-xs text-muted-foreground">
          <CheckCircle2 className="size-3.5 text-emerald-600" />
          Session history, artifacts and approvals are scoped to your account.
        </div>
      </section>

      <section className="relative flex flex-1 items-center justify-center p-5 sm:p-8">
        <div className="w-full max-w-[420px]">
          <div className="mb-6 flex justify-center lg:hidden">
            <Brand />
          </div>
          <Card className="border-border/70 bg-card/85 shadow-[0_24px_80px_-32px_oklch(0.2_0.03_270/0.35)] backdrop-blur-xl">
            <CardHeader className="gap-2 p-6 pb-5">
              <div className="mb-2 flex size-10 items-center justify-center rounded-xl border border-border/70 bg-background shadow-sm">
                {activeMode === "install" ? (
                  <ShieldCheck className="size-5 text-primary" />
                ) : (
                  <KeyRound className="size-5 text-primary" />
                )}
              </div>
              <CardTitle className="text-xl tracking-[-0.02em]">
                {activeMode === "install"
                  ? "Set up Morph"
                  : activeMode === "signup"
                    ? "Create your account"
                    : "Welcome back"}
              </CardTitle>
              <CardDescription>
                {activeMode === "install"
                  ? "Create the first operator account for this runtime."
                  : activeMode === "signup"
                    ? "Register a new operator account."
                    : "Sign in to open your runtime console."}
              </CardDescription>
            </CardHeader>
            <CardContent className="px-6 pb-6">
              {health.isPending ? (
                <div className="flex items-center gap-2 py-8 text-sm text-muted-foreground">
                  <Loader2 className="size-4 animate-spin" />
                  Checking runtime…
                </div>
              ) : health.isError ? (
                <div className="rounded-lg border border-destructive/30 bg-destructive/5 p-4 text-sm">
                  <div className="font-medium text-destructive">Runtime is unreachable</div>
                  <p className="mt-1 text-muted-foreground">{errorText(health.error)}</p>
                  <Button
                    className="mt-3"
                    variant="outline"
                    size="sm"
                    onClick={() => void health.refetch()}
                  >
                    Retry
                  </Button>
                </div>
              ) : (
                <form className="space-y-4" onSubmit={handleSubmit}>
                  <label className="block space-y-1.5" htmlFor="auth-username">
                    <span className="text-xs font-medium text-muted-foreground">Username</span>
                    <div className="relative">
                      <UserRound className="pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
                      <Input
                        id="auth-username"
                        name="username"
                        autoComplete="username"
                        autoFocus
                        className="pl-9"
                        placeholder="operator"
                        value={username}
                        onChange={(event) => setUsername(event.target.value)}
                      />
                    </div>
                  </label>
                  <label className="block space-y-1.5" htmlFor="auth-password">
                    <span className="text-xs font-medium text-muted-foreground">Password</span>
                    <div className="relative">
                      <LockKeyhole className="pointer-events-none absolute left-3 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
                      <Input
                        id="auth-password"
                        name="password"
                        autoComplete={activeMode === "login" ? "current-password" : "new-password"}
                        className="pl-9"
                        placeholder="••••••••"
                        type="password"
                        value={password}
                        onChange={(event) => setPassword(event.target.value)}
                      />
                    </div>
                  </label>
                  {activeMode !== "login" && (
                    <label className="block space-y-1.5" htmlFor="auth-confirm">
                      <span className="text-xs font-medium text-muted-foreground">
                        Confirm password
                      </span>
                      <Input
                        id="auth-confirm"
                        name="confirm"
                        autoComplete="new-password"
                        placeholder="••••••••"
                        type="password"
                        value={confirm}
                        onChange={(event) => setConfirm(event.target.value)}
                      />
                    </label>
                  )}
                  {activeMode === "signup" && (
                    <label className="block space-y-1.5" htmlFor="auth-signup-code">
                      <span className="text-xs font-medium text-muted-foreground">
                        Signup code <span className="font-normal">(if required)</span>
                      </span>
                      <Input
                        id="auth-signup-code"
                        name="signupCode"
                        placeholder="Optional"
                        value={signupCode}
                        onChange={(event) => setSignupCode(event.target.value)}
                      />
                    </label>
                  )}
                  {error && (
                    <div className="rounded-lg border border-destructive/30 bg-destructive/5 px-3 py-2 text-xs leading-5 text-destructive">
                      {error}
                    </div>
                  )}
                  <Button className="w-full" disabled={submitting} type="submit">
                    {submitting ? (
                      <Loader2 className="size-4 animate-spin" />
                    ) : (
                      <ArrowRight className="size-4" />
                    )}
                    {activeMode === "install"
                      ? "Create operator account"
                      : activeMode === "signup"
                        ? "Create account"
                        : "Sign in"}
                  </Button>
                  {!setupRequired && (
                    <div className="text-center text-xs text-muted-foreground">
                      {activeMode === "login" ? (
                        <>
                          Need an account?{" "}
                          <button
                            className="font-medium text-foreground underline-offset-4 hover:underline"
                            type="button"
                            onClick={() => switchMode("signup")}
                          >
                            Sign up
                          </button>
                        </>
                      ) : (
                        <>
                          Already registered?{" "}
                          <button
                            className="font-medium text-foreground underline-offset-4 hover:underline"
                            type="button"
                            onClick={() => switchMode("login")}
                          >
                            Sign in
                          </button>
                        </>
                      )}
                    </div>
                  )}
                </form>
              )}
            </CardContent>
          </Card>
          <div className="mt-5 flex items-center justify-center gap-2 text-[11px] text-muted-foreground">
            <BrandMark className="size-5 rounded-md" />
            Token-based auth · stored locally in this browser
          </div>
        </div>
      </section>
    </div>
  );
}
