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
  const MAX_MARKDOWN_BYTES = 256 * 1024;
  const MAX_MERMAID_BYTES = 64 * 1024;
  const MAX_MERMAID_NODES = 256;
  const MAX_MATH_BYTES = 4096;
  const RENDER_TIMEOUT_MS = 2000;
  const initialQuery = new URLSearchParams(window.location.search);
  const screenshotMode = initialQuery.get("screenshot") === "1" || initialQuery.get("view") === "operations";
  let activeAssistant = null;
  let busy = false;
  let turnCount = 0;
  let interactionSnapshot = null;
  let selectedInteraction = null;

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
        const expression = match[2] == null ? match[3] : match[2];
        try {
          if (expression.length > MAX_MATH_BYTES || /\\(?:write18|input|include|openout|read)\b/i.test(expression)) {
            throw new Error("math safety limit");
          }
          window.katex.render(expression, span, {
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
      const source = code.textContent || "";
      const complexity = source.split(/\r?\n/).filter((line) => line.trim()).length +
        (source.match(/-->/g) || []).length;
      if (source.length > MAX_MERMAID_BYTES || complexity > MAX_MERMAID_NODES ||
          /%%\{|\b(?:click|href|link)\s|<script|javascript:/i.test(source)) {
        code.parentElement.classList.add("render-error");
        return;
      }
      const diagram = document.createElement("div");
      diagram.className = "mermaid";
      diagram.textContent = source;
      code.parentElement.replaceWith(diagram);
      nodes.push(diagram);
    });
    if (!nodes.length) return;
    let timer;
    try {
      await Promise.race([
        window.mermaid.run({ nodes, suppressErrors: true }),
        new Promise((_, reject) => { timer = window.setTimeout(() => reject(new Error("render timeout")), RENDER_TIMEOUT_MS); })
      ]);
    }
    catch (_) { nodes.forEach((node) => node.classList.add("render-error")); }
    finally { if (timer) window.clearTimeout(timer); }
  }

  function renderMarkdown(target, source, finalRender) {
    const raw = text(source);
    const bounded = raw.length <= MAX_MARKDOWN_BYTES ? raw : raw.slice(0, MAX_MARKDOWN_BYTES) + "\n\n[content truncated]";
    target.replaceChildren(safeFragment(markdown.render(bounded)));
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
    article.dataset.interactive = "true";
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
    article.addEventListener("click", function (event) {
      if (event.target.closest("a, button, details")) return;
      openInspector(role === "user" ? "message" : "thinking");
    });
    thinkingSummary.addEventListener("click", function () { window.setTimeout(function(){ openInspector("thinking"); },0); });
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

  function statusLabel(value) { const v = text(value || "unknown"); return v.charAt(0).toUpperCase() + v.slice(1); }
  function nodesOf(kind) { return interactionSnapshot ? (interactionSnapshot.nodes || []).filter(function(n){ return n.kind === kind || (kind === "thinking" && (n.kind === "plan" || n.kind === "plan_node")) || (kind === "agent" && n.kind === "skill_node") || (kind === "evidence" && (n.kind === "finding" || n.kind === "artifact")); }) : []; }
  function sourceLine(node) { const source=node.source||{}; return text(source.store)+" / "+text(source.object_id)+" · r"+Number(source.revision||0)+" · "+text(source.digest); }
  function inspectorKind(nodeKind) { if(["plan","plan_node","cognition_stage"].includes(nodeKind))return "thinking";if(nodeKind==="skill_node")return "agent";if(["finding","artifact"].includes(nodeKind))return "evidence";return nodeKind; }
  function renderInspector(kind, objectId) {
    const content=$("inspector-content"); content.replaceChildren();
    const candidates=nodesOf(kind); selectedInteraction=candidates.find(function(n){return n.node_id===objectId;})||candidates[0]||null;
    $("inspector-title").textContent=kind === "thinking" ? "Cognition & executable plan" : kind === "memory_view" ? "Five-layer memory view" : kind === "agent" ? "Collaborative agents & skills" : kind === "approval" ? "Accountable approval" : kind === "evidence" ? "Assurance, findings & artifacts" : "Conversation source";
    document.querySelectorAll("[data-inspector-kind]").forEach(function(button){button.classList.toggle("selected",button.dataset.inspectorKind===kind);});
    $("inspector-provenance").textContent=selectedInteraction ? sourceLine(selectedInteraction) : "No canonical source revision is available";
    candidates.forEach(function(node){const card=document.createElement("article");card.className="inspector-card"+(node===selectedInteraction?" selected":"");card.dataset.nodeId=node.node_id;
      const title=document.createElement("h3");title.textContent=text(node.label||node.node_id);const state=statusNode(node.state);const summary=document.createElement("p");summary.textContent=text(node.summary);
      const detail=document.createElement("pre");detail.textContent=pretty(node.display||{});card.append(title,state,summary,detail);card.addEventListener("click",function(){renderInspector(kind,node.node_id);});content.append(card);});
    if(!candidates.length){const empty=document.createElement("p");empty.className="ops-note";empty.textContent="No view-safe "+kind+" object is attached to this turn.";content.append(empty);}
  }
  function openInspector(kind, objectId) { $("interaction-inspector").hidden=false;renderInspector(kind,objectId);if(selectedInteraction)history.replaceState(null,"","#inspect="+encodeURIComponent(selectedInteraction.node_id)); }
  function loadInteractionSnapshot() { return fetch("/ui/interactions/snapshot",{cache:"no-store"}).then(function(response){if(!response.ok)throw new Error("interaction snapshot HTTP "+response.status);return response.json();}).then(function(snapshot){interactionSnapshot=snapshot;
      if(screenshotMode && turnCount===0){const message=(snapshot.nodes||[]).find(function(n){return n.kind==="message";});const plan=(snapshot.nodes||[]).find(function(n){return n.kind==="plan";});
        if(message)addTurn("user",message.summary,false);if(plan){const answer=addTurn("assistant","## Harness-linked result\n\n"+text(plan.summary)+"\n\nPlan revision **"+Number((plan.ref||{}).plan_revision||plan.revision||0)+"** is connected to cognition, governed memory, delegated agents, approval and assurance evidence.",false);answer.rawThinking=text(((snapshot.nodes||[]).find(function(n){return n.kind==="thinking";})||{}).summary);answer.thinking.hidden=false;answer.thinkingBody.textContent=answer.rawThinking;}}
      const hash=new URLSearchParams(location.hash.replace(/^#/,""));const id=hash.get("inspect");if(id){const n=(snapshot.nodes||[]).find(function(item){return item.node_id===id;});if(n)openInspector(inspectorKind(n.kind),n.node_id);}}); }
  function statusNode(value) {
    const span = document.createElement("span"); span.className = "ops-state";
    span.dataset.state = text(value || "unknown"); span.textContent = statusLabel(value); return span;
  }
  function cell(row, value) {
    const td = document.createElement("td");
    if (value instanceof Node) td.append(value); else td.textContent = text(value);
    row.append(td); return td;
  }
  function renderOperations(snapshot) {
    if (!snapshot || snapshot.schema_version !== "phase4.operations.v1") return;
    $("ops-empty").hidden = true; $("ops-content").hidden = false;
    $("operations-badge").textContent = statusLabel(snapshot.overall_status);
    $("operations-badge").dataset.state = text(snapshot.overall_status);
    $("ops-closure").textContent = snapshot.task_completion_verified ? "VERIFIED" : "UNVERIFIED";
    $("ops-closure").dataset.state = snapshot.task_completion_verified ? "passed" : "warning";
    $("ops-authority").textContent = text(snapshot.task_closure_state || "running") + " · authority: " + text(snapshot.completion_authority || "none");
    const updated = $("ops-updated");
    if (updated) updated.textContent = text(snapshot.updated_at || "—");
    $("ops-revision").textContent = "r" + Number(snapshot.plan_revision || 0);
    $("ops-run").textContent = "task " + text(snapshot.task_id || "—") +
      " · run " + text(snapshot.run_id || "—") +
      " · turn " + text(snapshot.turn_id || "—");
    $("ops-criteria").textContent = Number(snapshot.criteria_closed || 0) + " / " + Number(snapshot.criteria_total || 0);
    $("ops-progress").textContent = "delta " + Number(snapshot.progress_delta || 0) + " · stagnant " + Number(snapshot.stagnation_count || 0);
    $("ops-live").textContent = text(snapshot.live_certification || "Unknown");
    $("ops-blocker").textContent = text(snapshot.blocker || "No blocker");
    $("ops-summary").textContent = text(snapshot.summary || "");

    const stages = $("ops-stages"); stages.replaceChildren();
    (snapshot.stages || []).forEach(function (stage, index) {
      const card = document.createElement("article"); card.className = "stage-card"; card.dataset.state = text(stage.status);
      const order = document.createElement("span"); order.className = "stage-order"; order.textContent = String(index + 1).padStart(2, "0");
      const body = document.createElement("div"); const head = document.createElement("div"); head.className = "stage-head";
      const title = document.createElement("b"); title.textContent = text(stage.label); head.append(title, statusNode(stage.status));
      const meta = document.createElement("small"); meta.textContent = text(stage.role || "system") + " · revision " + Number(stage.revision || 0) + " · " + (stage.evidence_ids || []).length + " evidence";
      const summary = document.createElement("p"); summary.textContent = text(stage.summary || "No displayable summary");
      body.append(head, meta, summary); card.append(order, body); stages.append(card);
    });

    const agents = $("ops-agents"); agents.replaceChildren();
    (snapshot.agent_templates || []).forEach(function (agent) {
      const card = document.createElement("article"); card.className = "agent-runtime-card";
      card.addEventListener("click", function(){ openInspector("agent", "agent:"+text(agent.invocation_id)); });
      const head = document.createElement("div"); head.className = "stage-head";
      const title = document.createElement("b"); title.textContent = text(agent.template_id) + " · r" + Number(agent.template_revision || 0);
      const mode = document.createElement("span"); mode.className = "runtime-mode"; mode.textContent = text(agent.business_mode) + " / " + text(agent.hosting_mode);
      head.append(title, mode);
      const plan = document.createElement("p"); plan.textContent = "Plan " + text(agent.plan_id) + " · r" + Number(agent.plan_revision || 0) + " · session " + text(agent.session_id);
      const pins = document.createElement("small"); pins.textContent = "registry " + text(agent.registry_generation || "—") + " · deployment " + text(agent.deployment_generation || "—") + " · closure " + text(agent.completion_authority || "none") + ": " + text(agent.completion_reason || "pending");
      card.append(head, plan, pins); agents.append(card);
    });
    if (!(snapshot.agent_templates || []).length) { const empty = document.createElement("p"); empty.className = "ops-note-inline"; empty.textContent = "No AgentTemplate invocation attached to this snapshot."; agents.append(empty); }
    const skillNodes = $("ops-skill-nodes"); skillNodes.replaceChildren();
    (snapshot.skill_nodes || []).forEach(function (node) {
      const row = document.createElement("tr");
      const identity = document.createElement("div"); const name = document.createElement("b"); name.textContent = text(node.node_id); const role = document.createElement("small"); role.textContent = text(node.role); identity.append(name, role);
      cell(row, identity); cell(row, text(node.skill_id) + "\n" + text(node.skill_version)); cell(row, node.runner); cell(row, statusNode(node.state)); cell(row, (node.evidence_refs || []).length + " evidence · " + (node.artifact_refs || []).length + " artifacts"); skillNodes.append(row);
    });

    const memory = $("ops-memory"); memory.replaceChildren();
    (snapshot.memory || []).forEach(function (item) {
      const row = document.createElement("tr"); const source = document.createElement("div");
      row.addEventListener("click", function(){ openInspector("memory_view"); });
      const scope = document.createElement("b"); scope.textContent = text(item.scope);
      const small = document.createElement("small"); small.textContent = text(item.source); source.append(scope, small);
      cell(row, source); cell(row, item.authority); cell(row, item.freshness);
      const decision = document.createElement("div"); decision.append(statusNode(item.selected ? "passed" : "unknown"));
      const reason = document.createElement("small"); reason.textContent = (item.selected ? "selected · " : "excluded · ") + text(item.selection_reason); decision.append(reason); cell(row, decision); memory.append(row);
    });

    const assurance = $("ops-assurance"); assurance.replaceChildren();
    (snapshot.assurance || []).forEach(function (layer) {
      const row = document.createElement("div"); row.className = "assurance-row"; const name = document.createElement("div");
      row.addEventListener("click", function(){ openInspector("evidence", (layer.finding_ids || []).length ? "finding:"+text(layer.finding_ids[0]) : undefined); });
      const b = document.createElement("b"); b.textContent = text(layer.label); const small = document.createElement("small");
      small.textContent = text(layer.oracle || "no oracle") + " · " + text(layer.verifier || "no verifier");
      name.append(b, small); row.append(name, statusNode(layer.status)); assurance.append(row);
    });

    const invocations = $("ops-invocations"); invocations.replaceChildren();
    (snapshot.invocations || []).forEach(function (item) {
      const row = document.createElement("tr"); const who = document.createElement("div");
      const b = document.createElement("b"); b.textContent = text(item.role); const small = document.createElement("small"); small.textContent = text(item.provider) + " / " + text(item.model); who.append(b, small);
      cell(row, who); cell(row, text(item.prompt_version) + "\n" + text(item.view_id));
      cell(row, Number(item.input_tokens || 0).toLocaleString() + " / " + Number(item.output_tokens || 0).toLocaleString());
      cell(row, "$" + Number(item.cost_usd || 0).toFixed(4)); cell(row, Number(item.latency_ms || 0).toLocaleString() + " ms"); cell(row, statusNode(item.status)); invocations.append(row);
    });

    const hitl = $("ops-hitl"); hitl.replaceChildren();
    (snapshot.hitl || []).forEach(function (request) {
      const card = document.createElement("article"); card.className = "hitl-card"; const body = document.createElement("div");
      card.addEventListener("click", function(event){ if(!event.target.closest("button"))openInspector("approval","approval:"+text(request.id)); });
      const head = document.createElement("div"); head.className = "hitl-head"; const title = document.createElement("b"); title.textContent = text(request.kind).replaceAll("_", " "); head.append(title, statusNode(request.status));
      const summary = document.createElement("p"); summary.textContent = text(request.summary); const meta = document.createElement("small"); meta.textContent = "Requested by " + text(request.requested_by || "system") + " · due " + text(request.deadline || "not set"); body.append(head, summary, meta);
      const actions = document.createElement("div"); actions.className = "hitl-actions";
      if (request.status === "pending") (request.allowed_actions || []).forEach(function (action) { const button = document.createElement("button"); button.type = "button"; button.textContent = text(action).replaceAll("_", " "); button.addEventListener("click", function () { submitHitl(request.id, action, button); }); actions.append(button); });
      card.append(body, actions); hitl.append(card);
    });
  }

  async function submitHitl(requestId, action, button) {
    button.disabled = true; $("hitl-feedback").textContent = "Submitting accountable decision…";
    try {
      const sessionToken = text(sessionStorage.getItem("agent_hitl_session_token")).trim();
      if (!sessionToken) throw new Error("authenticated HITL session is not configured");
      const response = await fetch("/ui/operations/hitl", { method: "POST", headers: { "Content-Type": "application/json", "X-Agent-Session": sessionToken }, body: JSON.stringify({ request_id: requestId, action }) });
      if (!response.ok) throw new Error(await response.text());
      $("hitl-feedback").textContent = "Decision recorded and snapshot refreshed";
    } catch (error) { $("hitl-feedback").textContent = "Decision not applied: " + text(error.message || error); }
    finally { button.disabled = false; }
  }

  function setWorkspace(view) {
    const ops = view === "operations"; $("conversation-view").hidden = ops; $("operations-view").hidden = !ops; composer.hidden = ops;
    $("clear-button").hidden = ops;
    $("conversation-tab").classList.toggle("selected", !ops); $("operations-tab").classList.toggle("selected", ops);
    $("conversation-tab").setAttribute("aria-selected", String(!ops)); $("operations-tab").setAttribute("aria-selected", String(ops));
  }

  function handleAux(o) {
    const type = text(o.type); const payload = o.payload || {};
    if (type === "runtime") {
      $("session-name").textContent = text(payload.session || "default");
      $("rail-session-name").textContent = text(payload.session || "default");
      $("provider-name").textContent = text(payload.provider || "OpenAI");
      $("model-name").textContent = text(payload.model || "provider default");
      setConnection(text(payload.connection || "Connected"), "connected");
      const executionPath = $("execution-path");
      const path = text(payload.execution_path || "harness");
      executionPath.textContent = path === "legacy_react_fallback" ? "UNVERIFIED FALLBACK" : path.toUpperCase();
      executionPath.dataset.state = path === "legacy_react_fallback" ? "warning" : "passed";
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
    } else if (type === "runtime_event" && payload.event_type === "turn_execution_path_selected") {
      const path = text((payload.payload || {}).path || "fail_closed");
      $("execution-path").textContent = path === "legacy_react_fallback" ? "UNVERIFIED FALLBACK" : path.toUpperCase();
      $("execution-path").dataset.state = path === "harness" ? "passed" : "warning";
    } else if (type === "phase4_operations") renderOperations(payload);
    else if (type === "interaction_snapshot") interactionSnapshot = payload;
    else if (type === "user_turn" || type === "demo_user") {
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

  function handleServerEvent(ev) {
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
  }
  function loadOperationsSnapshot() {
    return fetch("/ui/operations/snapshot").then(function (response) {
      if (!response.ok) throw new Error("operations snapshot HTTP " + response.status);
      return response.json();
    }).then(renderOperations);
  }
  function loadBootstrapSnapshot() {
    return fetch("/ui/bootstrap", { cache: "no-store" }).then(function (response) {
      if (!response.ok) throw new Error("bootstrap HTTP " + response.status);
      return response.json();
    }).then(function (snapshot) {
      handleAux({ type: "runtime", payload: snapshot.runtime || {} });
      handleAux({ type: "skills_status", payload: snapshot.skills_status || {} });
    });
  }
  if (!screenshotMode) {
    loadBootstrapSnapshot().catch(function () { setStatus("Bootstrap snapshot unavailable; waiting for event stream…"); });
    loadOperationsSnapshot().catch(function () {
      setStatus("Operations snapshot unavailable; waiting for event stream…");
    });
    loadInteractionSnapshot().catch(function(){ setStatus("Interaction projection unavailable; canonical operations remain visible"); });
    const es = new EventSource("/ui/sse?session=default");
    es.onopen = function () { setConnection("Connected", "connected"); setStatus("Event stream connected"); };
    es.onerror = function () { setConnection("Reconnecting", "error"); setStatus("Event stream interrupted; reconnecting…"); };
    es.onmessage = handleServerEvent;
  } else {
    loadOperationsSnapshot()
      .then(loadInteractionSnapshot)
      .then(function () { setConnection("Snapshot", "connected"); setStatus("Deterministic screenshot state loaded"); });
  }

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
  $("conversation-tab").addEventListener("click", function () { setWorkspace("conversation"); });
  $("operations-tab").addEventListener("click", function () { setWorkspace("operations"); });
  $("rail-toggle").addEventListener("click", function () { $("left-rail").classList.toggle("open"); });
  $("activity-close").addEventListener("click", function () { $("activity-panel").classList.remove("open"); });
  $("inspector-close").addEventListener("click",function(){ $("interaction-inspector").hidden=true;history.replaceState(null,"",location.pathname+location.search); });
  document.querySelectorAll("[data-inspector-kind]").forEach(function(button){button.addEventListener("click",function(){openInspector(button.dataset.inspectorKind);});});
  $("inspector-origin").addEventListener("click",function(){const original=nodesOf("message")[0];if(!original)return;openInspector("message",original.node_id);const turn=document.querySelector('.turn[data-role="user"]');if(turn)turn.scrollIntoView({behavior:"smooth",block:"center"});});
  $("inspector-related").addEventListener("click",function(){if(!selectedInteraction||!interactionSnapshot)return;const edge=(interactionSnapshot.edges||[]).find(function(e){return e.from_node_id===selectedInteraction.node_id||e.to_node_id===selectedInteraction.node_id;});if(!edge)return;const id=edge.from_node_id===selectedInteraction.node_id?edge.to_node_id:edge.from_node_id;const node=(interactionSnapshot.nodes||[]).find(function(n){return n.node_id===id;});if(node)openInspector(inspectorKind(node.kind),node.node_id);});
  $("command-button").addEventListener("click", function () { $("command-dialog").showModal(); $("command-input").focus(); });
  document.addEventListener("keydown", function (event) {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") { event.preventDefault(); $("command-dialog").showModal(); $("command-input").focus(); }
    if ((event.ctrlKey || event.metaKey) && event.key === ".") { event.preventDefault(); stopBtn.click(); }
  });
  updateCounts();
  if (initialQuery.get("view") === "operations")
    setWorkspace("operations");
  else promptEl.focus();
})();
