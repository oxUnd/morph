import { CornerDownLeft, Loader2, Send } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { Button } from "@/components/ui/button";
import { Textarea } from "@/components/ui/textarea";

export function Composer({
  disabled,
  placeholder = "Message Morph…",
  onSend,
}: {
  disabled: boolean;
  placeholder?: string;
  onSend: (text: string) => Promise<void>;
}) {
  const [value, setValue] = useState("");
  const [sending, setSending] = useState(false);
  const textareaRef = useRef<HTMLTextAreaElement>(null);

  // biome-ignore lint/correctness/useExhaustiveDependencies: value changes must resize the textarea
  useEffect(() => {
    const textarea = textareaRef.current;
    if (!textarea) return;
    textarea.style.height = "auto";
    textarea.style.height = `${Math.min(textarea.scrollHeight, 180)}px`;
  }, [value]);

  async function submit() {
    const text = value.trim();
    if (!text || disabled || sending) return;
    setSending(true);
    setValue("");
    try {
      await onSend(text);
    } catch {
      setValue(text);
    } finally {
      setSending(false);
    }
  }

  return (
    <div className="rounded-2xl border border-border/70 bg-card/90 shadow-[0_14px_40px_-30px_oklch(0.4_0.03_280/0.55)] backdrop-blur transition-[border-color,box-shadow] focus-within:border-primary/35 focus-within:shadow-[0_18px_48px_-30px_oklch(0.5_0.18_280/0.55)]">
      <Textarea
        ref={textareaRef}
        aria-label="Message"
        className="min-h-[68px] resize-none border-0 bg-transparent px-4 pt-3.5 text-[13.5px] leading-6 shadow-none focus-visible:ring-0"
        disabled={disabled}
        placeholder={placeholder}
        rows={2}
        value={value}
        onChange={(event) => setValue(event.target.value)}
        onKeyDown={(event) => {
          if (event.key === "Enter" && !event.shiftKey && !event.nativeEvent.isComposing) {
            event.preventDefault();
            void submit();
          }
        }}
      />
      <div className="flex items-center gap-2 px-3 pb-2.5 pt-0.5">
        <span className="hidden items-center gap-1 text-[10px] text-muted-foreground sm:inline-flex">
          <kbd className="rounded border border-border/70 bg-muted/50 px-1 py-0.5 font-mono text-[9px]">
            Enter
          </kbd>
          to send
          <span className="mx-0.5 text-border">·</span>
          <kbd className="rounded border border-border/70 bg-muted/50 px-1 py-0.5 font-mono text-[9px]">
            Shift + Enter
          </kbd>
          for newline
        </span>
        <Button
          aria-label="Send message"
          className="ml-auto size-8 rounded-xl"
          disabled={disabled || sending || !value.trim()}
          size="icon"
          type="button"
          onClick={() => void submit()}
        >
          {sending ? <Loader2 className="animate-spin" /> : <Send />}
          {!sending && <CornerDownLeft className="hidden" />}
        </Button>
      </div>
    </div>
  );
}
