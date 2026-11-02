import type { ReactNode } from "react";
import { useAuth } from "@/lib/auth";
import { AuthScreen } from "./auth-screen";

export function AuthGate({ children }: { children: ReactNode }) {
  const auth = useAuth();
  if (!auth.token) return <AuthScreen />;
  return <>{children}</>;
}
