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
  let activeAssistant = null;
  let busy = false;
  let turnCount = 0;

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

  function addTurn(role, content, streaming) {
    if (emptyState && emptyState.isConnected) emptyState.remove();
    const article = document.createElement("article");
    article.className = "turn" + (streaming ? " streaming" : "");
    article.dataset.role = role;
    const marker = document.createElement("div"); marker.className = "turn-marker";
    marker.textContent = role === "assistant" ? "A" : role === "user" ? "U" : "!";
    const contentWrap = document.createElement("div");
    const head = document.createElement("div"); head.className = "turn-head";
    const strong = document.createElement("strong"); strong.textContent = role === "assistant" ? "Agent" : role === "user" ? "You" : "System";
    const time = document.createElement("small"); time.textContent = nowLabel();
    const body = document.createElement("pre"); body.className = "turn-body"; body.textContent = text(content);
    head.append(strong, time); contentWrap.append(head, body); article.append(marker, contentWrap);
    conversation.appendChild(article); turnCount += 1; updateCounts(); scrollConversation();
    return { article, body };
  }

  function ensureAssistant() {
    if (!activeAssistant) activeAssistant = addTurn("assistant", "", true);
    return activeAssistant;
  }
  function finishAssistant() {
    if (activeAssistant) activeAssistant.article.classList.remove("streaming");
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

  function handleAux(o) {
    const type = text(o.type); const payload = o.payload || {};
    if (type === "runtime") {
      $("session-name").textContent = text(payload.session || "default");
      $("rail-session-name").textContent = text(payload.session || "default");
      $("provider-name").textContent = text(payload.provider || "OpenAI");
      $("model-name").textContent = text(payload.model || "provider default");
      setConnection(text(payload.connection || "Connected"), "connected");
    } else if (type === "user_turn" || type === "demo_user") {
      addTurn("user", payload.content || payload.prompt || "", false); setBusy(true);
    } else if (type === "tool_started" || type === "tool_completed") {
      updateTool(type, payload);
    } else if (type === "run_cancelled") {
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
      ensureAssistant().body.textContent += o.content; scrollConversation();
    } else if (o.kind === "final") {
      if (!activeAssistant && o.final_answer) addTurn("assistant", o.final_answer, false);
      finishAssistant(); setBusy(false); setRunState("completed"); setStatus("Run completed");
    } else if (o.kind === "error") {
      const message = o.message || "Unknown error";
      if (activeAssistant && activeAssistant.body.textContent.includes(message)) {
        activeAssistant.article.classList.add("error");
      } else {
        addTurn("system", message, false);
      }
      finishAssistant(); setBusy(false); setRunState("failed"); setStatus("Run failed");
    } else if (o.kind === "aux") handleAux(o);
  };

  async function submitPrompt(prompt) {
    addTurn("user", prompt, false); activeAssistant = null; setBusy(true); setStatus("Submitting task…");
    try {
      const r = await fetch("/ui/run", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ prompt }) });
      if (r.status !== 202) throw new Error("HTTP " + r.status + " " + await r.text());
      promptEl.value = ""; setStatus("Agent is working…");
    } catch (error) {
      addTurn("system", error.message || error, false); setBusy(false); setRunState("failed");
    }
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
