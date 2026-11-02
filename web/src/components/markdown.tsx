import ReactMarkdown, { type Components } from "react-markdown";
import rehypeSanitize from "rehype-sanitize";
import remarkGfm from "remark-gfm";
import { cn } from "@/lib/utils";

const components: Components = {
  a: ({ node: _node, ...props }) => <a {...props} rel="noreferrer" target="_blank" />,
  pre: ({ node: _node, ...props }) => <pre {...props} />,
  table: ({ node: _node, ...props }) => (
    <div className="my-3 overflow-x-auto rounded-lg border border-border/70">
      <table {...props} />
    </div>
  ),
};

export function Markdown({ content, className }: { content: string; className?: string }) {
  return (
    <div className={cn("prose-morph min-w-0", className)}>
      <ReactMarkdown
        components={components}
        rehypePlugins={[rehypeSanitize]}
        remarkPlugins={[remarkGfm]}
      >
        {content}
      </ReactMarkdown>
    </div>
  );
}
