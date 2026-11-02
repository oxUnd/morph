import { describe, expect, it } from "vitest";
import { parseSSEBlock, parseSSEData, SSEParser } from "@/lib/sse";

describe("parseSSEBlock", () => {
  it("parses id, event and multiline data", () => {
    const frame = parseSSEBlock('id: 42\nevent: thought\ndata: {"text":\ndata: "hello"}');

    expect(frame).toEqual({
      id: 42,
      event: "thought",
      data: '{"text":\n"hello"}',
    });
  });

  it("normalizes CRLF and ignores comments", () => {
    const frame = parseSSEBlock(": keepalive\r\nid: 7\r\nevent: final\r\ndata: done\r\n");

    expect(frame).toEqual({ id: 7, event: "final", data: "done" });
  });

  it("returns null when a block has no data field", () => {
    expect(parseSSEBlock("id: 9\nevent: ping")).toBeNull();
    expect(parseSSEBlock(": keepalive\n\n")).toBeNull();
  });
});

describe("SSEParser", () => {
  it("emits frames across chunk boundaries", () => {
    const parser = new SSEParser();

    expect(parser.push("id: 1\ndata: hel")).toEqual([]);
    expect(parser.push("lo\n\nid: 2\nevent: final\ndata: bye\n\n")).toEqual([
      { id: 1, event: "message", data: "hello" },
      { id: 2, event: "final", data: "bye" },
    ]);
  });

  it("flushes a final frame without a trailing blank line", () => {
    const parser = new SSEParser();
    parser.push("data: tail");

    expect(parser.flush()).toEqual([{ id: 0, event: "message", data: "tail" }]);
    expect(parser.flush()).toEqual([]);
  });
});

describe("parseSSEData", () => {
  it("parses JSON and preserves non-JSON payloads as text", () => {
    expect(parseSSEData('{"turn_id":"t1"}')).toEqual({ turn_id: "t1" });
    expect(parseSSEData("plain text")).toEqual({ text: "plain text" });
  });
});
