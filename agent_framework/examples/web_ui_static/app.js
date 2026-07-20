(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const conversation = $("conversation");
  const emptyState = $("empty-state");
  const activityList = $("activity-list");
  const activityEmpty = $("activity-empty");
  const promptEl = $("prompt");
  const sendBtn = $("send");
  const stopBtn = $("stop");
  const composer = $("composer");
  const tools = new Map();
  const markdown = window.markdownit({ html: false, linkify: true, typographer: false });
  const renderTimers = new WeakMap();
  let activeAssistant = null;
  let busy = false;
  let turnCount = 0;

  if (window.mermaid) {
    window.mermaid.initialize({ startOnLoad: false, securityLevel: "strict", theme: "dark" });
  }

  function text(value) { return value == null ? "" : String(value); }
  function nowLabel() { return new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }); }
  function pretty(value) {
    if (value == null) return "No details";
    try { return JSON.stringify(value, null, 2); } catch (_) { return text(value); }
  }
  function setStatus(message) { $("status-message").textContent = message; }
  function setConnection(label, state) {
    const el = $("connection-state"); el.textContent = label; el.dataset.state = state;
  }
  function setRunState(state) {
    $("run-state").textContent = state; $("run-state").dataset.state = state;
    conversation.setAttribute("aria-busy", state === "running" ? "true" : "false");
  }
  function setBusy(value) {
    busy = value; sendBtn.disabled = value; stopBtn.hidden = !value;
    setRunState(value ? "running" : "idle");
  }
  function updateCounts() {
    $("message-count").textContent = String(turnCount);
    $("tool-count").textContent = String(tools.size);
    $("activity-count").textContent = tools.size + (tools.size === 1 ? " event" : " events");
  }
  function scrollConversation() { conversation.scrollTop = conversation.scrollHeight; }

  function safeFragment(html) {
    const clean = window.DOMPurify.sanitize(html, {
      USE_PROFILES: { html: true },
      FORBID_TAGS: ["style", "form", "iframe", "object", "embed"],
      FORBID_ATTR: ["style"]
    });
    const parsed = new DOMParser().parseFromString(clean, "text/html");
    const fragment = document.createDocumentFragment();
    while (parsed.body.firstChild) fragment.appendChild(parsed.body.firstChild);
    return fragment;
  }

  function renderMath(root) {
    if (!window.katex) return;
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    const nodes = [];
    while (walker.nextNode()) {
      const parent = walker.currentNode.parentElement;
      if (parent && !parent.closest("pre, code, .mermaid, .katex")) nodes.push(walker.currentNode);
    }
    const pattern = /(\$\$([\s\S]+?)\$\$)|\$([^$\n]+?)\$/g;
    nodes.forEach((node) => {
      const source = node.nodeValue || "";
      pattern.lastIndex = 0;
      if (!pattern.test(source)) return;
      pattern.lastIndex = 0;
      const fragment = document.createDocumentFragment();
      let cursor = 0;
      let match;
      while ((match = pattern.exec(source))) {
        fragment.append(document.createTextNode(source.slice(cursor, match.index)));
        const span = document.createElement(match[2] == null ? "span" : "div");
        span.className = match[2] == null ? "math-inline" : "math-block";
        try {
          window.katex.render(match[2] == null ? match[3] : match[2], span, {
            displayMode: match[2] != null, throwOnError: false, strict: "warn", trust: false
          });
        } catch (_) { span.textContent = match[0]; }
        fragment.append(span);
        cursor = match.index + match[0].length;
      }
      fragment.append(document.createTextNode(source.slice(cursor)));
      node.replaceWith(fragment);
    });
  }

  async function renderMermaid(root) {
    if (!window.mermaid) return;
    const nodes = [];
    root.querySelectorAll("pre > code.language-mermaid").forEach((code) => {
      const diagram = document.createElement("div");
      diagram.className = "mermaid";
      diagram.textContent = code.textContent;
      code.parentElement.replaceWith(diagram);
      nodes.push(diagram);
    });
    if (!nodes.length) return;
    try { await window.mermaid.run({ nodes, suppressErrors: true }); }
    catch (_) { nodes.forEach((node) => node.classList.add("render-error")); }
  }

  function renderMarkdown(target, source, finalRender) {
    target.replaceChildren(safeFragment(markdown.render(text(source))));
    renderMath(target);
    if (finalRender) void renderMermaid(target);
  }

  function scheduleRender(turn, immediate) {
    const prior = renderTimers.get(turn.markdownBody);
    if (prior) window.clearTimeout(prior);
    const run = () => {
      renderTimers.delete(turn.markdownBody);
      renderMarkdown(turn.markdownBody, turn.rawAnswer, immediate);
      scrollConversation();
    };
    if (immediate) run();
    else renderTimers.set(turn.markdownBody, window.setTimeout(run, 72));
  }

  function addTurn(role, content, streaming) {
    if (emptyState && emptyState.isConnected) emptyState.remove();
    const article = document.createElement("article");
    article.className = "turn" + (streaming ? " streaming" : "");
    article.dataset.role = role;
    const marker = document.createElement("div"); marker.className = "turn-marker";
    marker.textContent = role === "assistant" ? "A" : role === "user" ? "U" : "!";
    const contentWrap = document.createElement("div"); contentWrap.className = "turn-content";
    const head = document.createElement("div"); head.className = "turn-head";
    const strong = document.createElement("strong"); strong.textContent = role === "assistant" ? "Agent" : role === "user" ? "You" : "System";
    const time = document.createElement("small"); time.textContent = nowLabel();
    const thinking = document.createElement("details"); thinking.className = "thinking"; thinking.hidden = true;
    const thinkingSummary = document.createElement("summary"); thinkingSummary.textContent = "Reasoning summary";
    const thinkingBody = document.createElement("div"); thinkingBody.className = "thinking-body";
    thinking.append(thinkingSummary, thinkingBody);
    const body = document.createElement("div"); body.className = "turn-body";
    const markdownBody = document.createElement("div"); markdownBody.className = "markdown-body";
    const artifacts = document.createElement("div"); artifacts.className = "turn-artifacts";
    body.append(markdownBody, artifacts);
    head.append(strong, time); contentWrap.append(head, thinking, body); article.append(marker, contentWrap);
    conversation.appendChild(article); turnCount += 1; updateCounts();
    const turn = { article, body, markdownBody, artifacts, thinking, thinkingBody, rawAnswer: text(content), rawThinking: "" };
    renderMarkdown(markdownBody, turn.rawAnswer, true);
    scrollConversation();
    return turn;
  }

  function ensureAssistant() {
    if (!activeAssistant) activeAssistant = addTurn("assistant", "", true);
    return activeAssistant;
  }
  function appendThinking(content) {
    const turn = ensureAssistant();
    turn.rawThinking += content;
    turn.thinking.hidden = false;
    turn.thinkingBody.textContent = turn.rawThinking;
  }
  function finishAssistant() {
    if (activeAssistant) {
      activeAssistant.article.classList.remove("streaming");
      scheduleRender(activeAssistant, true);
    }
    activeAssistant = null;
  }

  function toolKey(payload) { return text(payload.tool_call_id || payload.id || payload.tool_name || ("tool-" + tools.size)); }
  function updateTool(type, payload) {
    const key = toolKey(payload); let entry = tools.get(key);
    if (!entry) {
      if (activityEmpty && activityEmpty.isConnected) activityEmpty.remove();
      const section = document.createElement("section"); section.className = "tool-event"; section.dataset.state = "running";
      const title = document.createElement("div"); title.className = "tool-title";
      const name = document.createElement("b"); name.textContent = text(payload.tool_name || "Tool");
      const state = document.createElement("span"); state.className = "tool-state"; state.textContent = "running";
      const details = document.createElement("details"); details.className = "tool-details";
      const summary = document.createElement("summary"); summary.textContent = "Arguments and result";
      const pre = document.createElement("pre");
      details.append(summary, pre); title.append(name, state); section.append(title, details); activityList.appendChild(section);
      entry = { section, state, pre, payload: {} }; tools.set(key, entry);
    }
    entry.payload = Object.assign({}, entry.payload, payload);
    const completed = type === "tool_completed" || Object.prototype.hasOwnProperty.call(payload, "result");
    const failed = completed && payload.result && (payload.result.error || payload.result.ok === false);
    const state = failed ? "failed" : completed ? "completed" : "running";
    entry.section.dataset.state = state; entry.state.textContent = state;
    entry.pre.textContent = "Arguments\n" + pretty(entry.payload.arguments || {}) + (completed ? "\n\nResult\n" + pretty(entry.payload.result || {}) : "");
    activityList.scrollTop = activityList.scrollHeight; updateCounts();
  }

  function addArtifact(payload) {
    if (!payload || typeof payload.path !== "string" || !payload.path || payload.path.startsWith("/") || payload.path.split("/").includes("..")) return;
    const turn = ensureAssistant();
    const figure = document.createElement("figure"); figure.className = "artifact";
    if (text(payload.mime).startsWith("image/")) {
      const image = document.createElement("img");
      image.loading = "lazy"; image.alt = text(payload.caption || "Generated artifact");
      image.src = "/ui/files/" + payload.path.split("/").map(encodeURIComponent).join("/");
      figure.append(image);
    }
    if (payload.caption) { const caption = document.createElement("figcaption"); caption.textContent = text(payload.caption); figure.append(caption); }
    turn.artifacts.append(figure); scrollConversation();
  }

  function handleAux(o) {
    const type = text(o.type); const payload = o.payload || {};
    if (type === "runtime") {
      $("session-name").textContent = text(payload.session || "default");
      $("rail-session-name").textContent = text(payload.session || "default");
      $("provider-name").textContent = text(payload.provider || "OpenAI");
      $("model-name").textContent = text(payload.model || "provider default");
      setConnection(text(payload.connection || "Connected"), "connected");
    } else if (type === "skills_status") {
      const enabled = payload.enabled === true;
      $("skills-state").textContent = enabled ? "READY" : "DISABLED";
      $("skills-health").textContent = enabled ? (Number(payload.errors || 0) ? "DEGRADED" : "READY") : "OFF";
      $("skills-count").textContent = String(payload.count || 0);
      $("skills-generation").textContent = String(payload.generation || 0);
      $("skills-errors").textContent = String(payload.errors || 0);
      $("skills-active").textContent = text(payload.active || "-");
      $("skills-root").textContent = enabled ? text(payload.root || "Unknown root") : "Skills disabled";
      $("skills-health").dataset.state = enabled && Number(payload.errors || 0) ? "error" : enabled ? "ready" : "off";
    } else if (type === "user_turn" || type === "demo_user") {
      addTurn("user", payload.content || payload.prompt || "", false); setBusy(true);
    } else if (type === "tool_started" || type === "tool_completed") updateTool(type, payload);
    else if (type === "artifact") addArtifact(payload);
    else if (type === "run_cancelled") {
      finishAssistant(); addTurn("system", payload.message || "Run cancelled", false); setBusy(false); setRunState("cancelled");
    } else if (type === "mcp_status") {
      const notice = addTurn("system", payload.message || "MCP status changed", false);
      if (payload.level === "error") notice.article.classList.add("error");
    }
  }

  const es = new EventSource("/ui/sse?session=default");
  es.onopen = function () { setConnection("Connected", "connected"); setStatus("Event stream connected"); };
  es.onerror = function () { setConnection("Reconnecting", "error"); setStatus("Event stream interrupted; reconnecting…"); };
  es.onmessage = function (ev) {
    let o; try { o = JSON.parse(ev.data); } catch (_) { return; }
    if (o.kind === "token" && typeof o.content === "string") {
      const turn = ensureAssistant(); turn.rawAnswer += o.content; scheduleRender(turn, false);
    } else if (o.kind === "thinking" && typeof o.content === "string") appendThinking(o.content);
    else if (o.kind === "final") {
      if (!activeAssistant && o.final_answer) addTurn("assistant", o.final_answer, false);
      else if (activeAssistant && !activeAssistant.rawAnswer && o.final_answer) activeAssistant.rawAnswer = text(o.final_answer);
      const summary = text(o.displayable_reasoning || o.reasoning_summary || "");
      if (activeAssistant && !activeAssistant.rawThinking && summary) appendThinking(summary);
      finishAssistant(); setBusy(false); setRunState("completed"); setStatus("Run completed");
    } else if (o.kind === "error") {
      const message = o.message || "Unknown error";
      if (activeAssistant && activeAssistant.rawAnswer.includes(message)) activeAssistant.article.classList.add("error");
      else addTurn("system", message, false);
      finishAssistant(); setBusy(false); setRunState("failed"); setStatus("Run failed");
    } else if (o.kind === "aux") handleAux(o);
  };

  async function submitPrompt(prompt) {
    addTurn("user", prompt, false); activeAssistant = null; setBusy(true); setStatus("Submitting task…");
    try {
      const r = await fetch("/ui/run", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ prompt }) });
      if (r.status !== 202) throw new Error("HTTP " + r.status + " " + await r.text());
      promptEl.value = ""; setStatus("Agent is working…");
    } catch (error) { addTurn("system", error.message || error, false); setBusy(false); setRunState("failed"); }
  }

  composer.addEventListener("submit", function (event) { event.preventDefault(); const prompt = promptEl.value.trim(); if (prompt && !busy) submitPrompt(prompt); });
  promptEl.addEventListener("keydown", function (event) { if (event.key === "Enter" && !event.shiftKey) { event.preventDefault(); composer.requestSubmit(); } });
  stopBtn.addEventListener("click", async function () {
    stopBtn.disabled = true;
    try { await fetch("/ui/cancel", { method: "POST" }); setStatus("Cancellation requested…"); }
    finally { stopBtn.disabled = false; }
  });
  $("clear-button").addEventListener("click", function () { conversation.replaceChildren(); tools.clear(); activityList.replaceChildren(); activeAssistant = null; turnCount = 0; updateCounts(); });
  $("rail-toggle").addEventListener("click", function () { $("left-rail").classList.toggle("open"); });
  $("activity-close").addEventListener("click", function () { $("activity-panel").classList.remove("open"); });
  $("command-button").addEventListener("click", function () { $("command-dialog").showModal(); $("command-input").focus(); });
  document.addEventListener("keydown", function (event) {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") { event.preventDefault(); $("command-dialog").showModal(); $("command-input").focus(); }
    if ((event.ctrlKey || event.metaKey) && event.key === ".") { event.preventDefault(); stopBtn.click(); }
  });
  updateCounts(); promptEl.focus();
})();
