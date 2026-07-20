# Web UI vendored dependencies

The Web demo is intentionally self-contained: runtime rendering does not fetch scripts, styles, or fonts from a CDN. Update these files only as a reviewed dependency change and keep the Content Security Policy in `index.html` restrictive.

| Package | Version | License | Upstream | Vendored content |
|---|---:|---|---|---|
| markdown-it | 14.1.0 | MIT | https://github.com/markdown-it/markdown-it | `markdown-it/markdown-it.min.js` |
| DOMPurify | 3.2.6 | Apache-2.0 OR MPL-2.0 | https://github.com/cure53/DOMPurify | `dompurify/purify.min.js` |
| KaTeX | 0.16.22 | MIT | https://github.com/KaTeX/KaTeX | `katex/katex.min.js`, `katex/katex.min.css`, `katex/fonts/*.woff2` |
| Mermaid | 10.9.3 | MIT | https://github.com/mermaid-js/mermaid | `mermaid/mermaid.min.js` |

## Integrity

```text
38c70a1e7ca91ab40e2d9e6e60129851a717ed1c7d4acbbdd41bf9503791cf68  markdown-it/markdown-it.min.js
89e1fa7647cb495370d3a997ace4387f5d15d9f4c5af12352c53daa400956287  dompurify/purify.min.js
e8d885505949f3a5f4abdd5dd0d53696bd1371ad26ffbf4f310dcd77c8cdae89  katex/katex.min.js
19095127357ed6d29fe0a63a6b000c913a89f7f1963b765dd3715e97c9852e75  katex/katex.min.css
5a8ec91820bd55afef049068489369910e5d6ce70c8103952f27e29d3e76e8bc  mermaid/mermaid.min.js
```

KaTeX font files are covered by the same KaTeX distribution and are required for offline formula rendering.
