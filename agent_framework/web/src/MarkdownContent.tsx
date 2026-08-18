import { useEffect, useId, useState } from "react";
import ReactMarkdown, { defaultUrlTransform } from "react-markdown";
import rehypeHighlight from "rehype-highlight";
import rehypeKatex from "rehype-katex";
import remarkGfm from "remark-gfm";
import remarkMath from "remark-math";

type MermaidState = { svg?: string; error?: string };

function MermaidDiagram({ source }: { source: string }) {
  const reactId = useId();
  const [state, setState] = useState<MermaidState>({});
  useEffect(() => {
    let current = true;
    const render = async () => {
      try {
        const mermaid = (await import("mermaid")).default;
        mermaid.initialize({
          startOnLoad: false,
          securityLevel: "strict",
          theme: "dark",
          suppressErrorRendering: true,
        });
        const id = `af-mermaid-${reactId.replace(/[^a-zA-Z0-9_-]/g, "")}`;
        const result = await mermaid.render(id, source);
        if (current) setState({ svg: result.svg });
      } catch (error) {
        if (current)
          setState({
            error: error instanceof Error ? error.message : "Invalid Mermaid diagram",
          });
      }
    };
    void render();
    return () => {
      current = false;
    };
  }, [reactId, source]);
  if (state.error)
    return (
      <figure className="mermaid-block render-error">
        <figcaption>Mermaid diagram could not be rendered</figcaption>
        <pre><code>{source}</code></pre>
        <small>{state.error}</small>
      </figure>
    );
  if (!state.svg)
    return <div className="mermaid-block loading" role="status">Rendering diagram…</div>;
  return (
    <figure
      className="mermaid-block"
      aria-label="Mermaid diagram"
      dangerouslySetInnerHTML={{ __html: state.svg }}
    />
  );
}

function safeUrl(value: string) {
  const transformed = defaultUrlTransform(value);
  return transformed || "";
}

export function MarkdownContent({ content }: { content: string }) {
  return (
    <div className="markdown-body">
      <ReactMarkdown
        remarkPlugins={[remarkGfm, remarkMath]}
        rehypePlugins={[rehypeKatex, rehypeHighlight]}
        urlTransform={safeUrl}
        components={{
          a({ href, children, ...props }) {
            const external = !!href && /^(?:https?:)?\/\//i.test(href);
            return (
              <a
                {...props}
                href={href}
                target={external ? "_blank" : undefined}
                rel={external ? "noopener noreferrer" : undefined}
              >
                {children}
              </a>
            );
          },
          img({ src, alt, ...props }) {
            return <img {...props} src={src} alt={alt || ""} loading="lazy" />;
          },
          code({ className, children, ...props }) {
            const source = String(children).replace(/\n$/, "");
            if (/\blanguage-mermaid\b/.test(className || ""))
              return <MermaidDiagram source={source} />;
            return <code {...props} className={className}>{children}</code>;
          },
        }}
      >
        {content}
      </ReactMarkdown>
    </div>
  );
}
