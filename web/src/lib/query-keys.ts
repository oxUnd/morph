export const queryKeys = {
  health: ["health"] as const,
  sessions: ["sessions"] as const,
  messages: (sessionId: string) => ["messages", sessionId] as const,
  artifact: (artifactId: string) => ["artifact", artifactId] as const,
};
