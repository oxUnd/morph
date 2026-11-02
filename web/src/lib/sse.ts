export interface SSEFrame {
  id: number;
  event: string;
  data: string;
}

function parseId(value: string): number {
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : 0;
}

export function parseSSEBlock(block: string): SSEFrame | null {
  const normalized = block.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
  if (!normalized.trim()) return null;

  let id = 0;
  let event = "message";
  const data: string[] = [];

  for (const line of normalized.split("\n")) {
    if (!line || line.startsWith(":")) continue;
    const colon = line.indexOf(":");
    const field = colon === -1 ? line : line.slice(0, colon);
    let value = colon === -1 ? "" : line.slice(colon + 1);
    if (value.startsWith(" ")) value = value.slice(1);

    if (field === "id") id = parseId(value);
    else if (field === "event") event = value || "message";
    else if (field === "data") data.push(value);
  }

  if (data.length === 0) return null;
  return { id, event, data: data.join("\n") };
}

export class SSEParser {
  private buffer = "";

  push(chunk: string): SSEFrame[] {
    this.buffer += chunk.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
    const frames: SSEFrame[] = [];
    let boundary = this.buffer.indexOf("\n\n");
    while (boundary !== -1) {
      const block = this.buffer.slice(0, boundary);
      this.buffer = this.buffer.slice(boundary + 2);
      const frame = parseSSEBlock(block);
      if (frame) frames.push(frame);
      boundary = this.buffer.indexOf("\n\n");
    }
    return frames;
  }

  flush(): SSEFrame[] {
    if (!this.buffer.trim()) {
      this.buffer = "";
      return [];
    }
    const frame = parseSSEBlock(this.buffer);
    this.buffer = "";
    return frame ? [frame] : [];
  }
}

export function parseSSEData(data: string): unknown {
  try {
    return JSON.parse(data);
  } catch {
    return { text: data };
  }
}
