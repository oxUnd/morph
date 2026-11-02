import { useEffect, useState } from "react";

export type Theme = "light" | "dark";

const THEME_KEY = "morph_theme";

function preferredTheme(): Theme {
  if (typeof window === "undefined") return "light";
  const stored = window.localStorage.getItem(THEME_KEY);
  if (stored === "light" || stored === "dark") return stored;
  return window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
}

function apply(theme: Theme) {
  if (typeof document === "undefined") return;
  document.documentElement.classList.toggle("dark", theme === "dark");
  document.documentElement.style.colorScheme = theme;
}

export function initTheme() {
  apply(preferredTheme());
}

export function useTheme() {
  const [theme, setThemeState] = useState<Theme>(preferredTheme);

  useEffect(() => {
    apply(theme);
    window.localStorage.setItem(THEME_KEY, theme);
  }, [theme]);

  return {
    theme,
    toggle: () => setThemeState((current) => (current === "dark" ? "light" : "dark")),
  };
}
