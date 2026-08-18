import { useEffect, useRef, useState } from "react";
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { api } from "./api";
import type { Capability, Run, RuntimeEvent, Session } from "./types";
import { initialLocale, translate, type Locale } from "./i18n";
import { MarkdownContent } from "./MarkdownContent";
import { RuntimePanel } from "./RuntimePanel";

const glyph: Record<string, string> = {
  understanding: "◈",
  decision: "?",
  plan: "⌁",
  plan_node: "·",
  tool_invocation: "⚙",
  agent: "◎",
  memory_view: "◇",
  approval: "!",
  evidence: "✓",
  artifact: "▣",
  closure: "◆",
};
function displayTime(value: string) {
  const timestamp = /^\d+$/.test(value) ? Number(value) : Date.parse(value);
  return Number.isFinite(timestamp)
    ? new Date(timestamp).toLocaleTimeString([], {
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit",
      })
    : value;
}
function SessionRail({
  items,
  active,
  search,
  onSearch,
  onSelect,
  onOpenPanel,
  open,
  locale,
}: {
  items: Session[];
  active?: string;
  search: string;
  onSearch: (value: string) => void;
  onSelect: (id: string) => void;
  onOpenPanel: (panel: "profile" | "settings") => void;
  open: boolean;
  locale: Locale;
}) {
  const t = (key: Parameters<typeof translate>[1]) => translate(locale, key);
  const client = useQueryClient();
  const profile = useQuery({ queryKey: ["runtime-profile"], queryFn: api.profile });
  const selected = items.find((item) => item.session_id === active);
  const [editor, setEditor] = useState<"new" | "rename" | "trash" | "purge" | null>(null);
  const [value, setValue] = useState("");
  const sessionMutation = useMutation({
    mutationFn: async (action: { kind: "new" | "rename" | "trash" | "restore" | "purge"; id: string; value?: string }) => {
      if (action.kind === "new") return api.createSession(action.value || "New Session", action.id);
      const current = items.find((item) => item.session_id === action.id);
      if (!current) throw new Error("Session is no longer available");
      if (action.kind === "rename") return api.renameSession(action.id, current.revision, action.value || current.title);
      if (action.kind === "trash") return api.transitionSession(action.id, current.revision, "trashed");
      if (action.kind === "restore") return api.restoreSession(action.id, current.revision);
      return api.purgeSession(action.id, current.revision, current.state);
    },
    onSuccess: async (_, action) => {
      setEditor(null); setValue("");
      await client.invalidateQueries({ queryKey: ["sessions"] });
      if (action.kind === "new") onSelect(action.id);
      if (action.kind === "purge") onSelect("");
    },
  });
  const openEditor = (mode: "new" | "rename" | "trash" | "purge") => {
    setEditor(mode);
    setValue(mode === "rename" ? selected?.title || "" : "");
  };
  const applyEditor = () => {
    if (editor === "new") {
      const id = `session-${crypto.randomUUID()}`;
      sessionMutation.mutate({ kind: "new", id, value: value.trim() || "New Session" });
    } else if (selected && editor === "rename") {
      sessionMutation.mutate({ kind: "rename", id: selected.session_id, value: value.trim() });
    } else if (selected && editor === "trash") {
      sessionMutation.mutate({ kind: "trash", id: selected.session_id });
    } else if (selected && editor === "purge" && value === selected.title) {
      sessionMutation.mutate({ kind: "purge", id: selected.session_id });
    }
  };
  return (
    <aside
      id="session-rail"
      className={`rail${open ? " open" : ""}`}
      aria-label={t("sessions")}
    >
      <div className="brand">
        <span className="brand-mark" aria-hidden="true">
          A
        </span>
        <div>
          <strong>Agent</strong>
          <small>{t("workbench")}</small>
        </div>
      </div>
      <label className="search">
        <span aria-hidden="true">⌕</span>
        <input
          aria-label={t("search")}
          placeholder={t("search")}
          value={search}
          onChange={(e) => onSearch(e.target.value)}
        />
      </label>
      <div className="section-label">
        {t("sessions")} <span>{items.length}</span>
      </div>
      <button className="new-session" onClick={() => openEditor("new")}>New Session</button>
      <nav aria-label={t("sessions")}>
        {items.map((item) => (
          <button
            key={item.session_id}
            aria-current={active === item.session_id ? "page" : undefined}
            className={
              active === item.session_id ? "session active" : "session"
            }
            onClick={() => onSelect(item.session_id)}
          >
            <span className="session-dot" aria-hidden="true" />
            <span>
              <strong>{item.title || item.session_id}</strong>
              <small>
                {item.state} · r{item.revision}
              </small>
            </span>
          </button>
        ))}
      </nav>
      {selected && (
        <div className="session-actions" aria-label="Selected Session actions">
          {selected.state !== "purged" && <button onClick={() => openEditor("rename")}>Rename</button>}
          {selected.state === "active" || selected.state === "archived" ? <button onClick={() => openEditor("trash")}>Trash</button> : null}
          {selected.state === "archived" || selected.state === "trashed" || selected.state === "purge_pending" ? <button onClick={() => sessionMutation.mutate({ kind: "restore", id: selected.session_id })}>Restore</button> : null}
          {(selected.state === "trashed" || selected.state === "purge_pending") && <button className="danger" onClick={() => openEditor("purge")}>Purge</button>}
        </div>
      )}
      {editor && (
        <div className="session-editor" role="dialog" aria-label={`${editor} Session`}>
          <strong>{editor === "new" ? "Create Session" : editor === "rename" ? "Rename Session" : editor === "trash" ? "Move Session to trash?" : "Permanently purge Session"}</strong>
          {(editor === "new" || editor === "rename" || editor === "purge") && <input autoFocus value={value} placeholder={editor === "purge" ? `Type “${selected?.title}”` : "Session title"} onChange={(event) => setValue(event.target.value)} />}
          {editor === "purge" && <small>This permanently removes the trashed Session. Type its exact title to continue.</small>}
          <div><button onClick={() => setEditor(null)}>Cancel</button><button className={editor === "purge" ? "danger" : "primary"} disabled={sessionMutation.isPending || (editor === "rename" && !value.trim()) || (editor === "purge" && value !== selected?.title)} onClick={applyEditor}>{editor === "new" ? "Create" : editor === "rename" ? "Save" : editor === "trash" ? "Move to trash" : "Permanently purge"}</button></div>
        </div>
      )}
      {sessionMutation.error && <p className="rail-error" role="alert">{sessionMutation.error.message}</p>}
      <div className="rail-tools">
        <button onClick={() => onOpenPanel("profile")}><span>{profile.data?.principal_id || "User"}</span><small>{profile.data?.organization_id || "Identity"}</small></button>
        <button onClick={() => onOpenPanel("settings")}><span>System settings</span><small>Runtime configuration</small></button>
      </div>
      <div className="rail-foot" role="status">
        <span className="status-dot" aria-hidden="true" /> {t("connected")}
      </div>
    </aside>
  );
}

function RunStrip({
  events,
  actions,
  run,
  onCancel,
  cancelling,
}: {
  events: RuntimeEvent[];
  actions: Capability[];
  run?: Run;
  onCancel: () => void;
  cancelling: boolean;
}) {
  const latest = events.at(-1),
    action = actions.find((a) => a.action_id === "run.cancel");
  const terminal =
    !run || ["completed", "failed", "cancelled"].includes(run.state);
  return (
    <section className="run-strip" aria-label="Run status" aria-live="polite">
      <div>
        <span
          className={terminal ? "status-dot" : "pulse"}
          aria-hidden="true"
        />
        <strong>{run ? `Run ${run.state}` : "No active run"}</strong>
        <small>
          {run?.run_id || latest?.run_id || "Select or start a Session"}
        </small>
      </div>
      <div className="run-meta">
        <span>
          Event head <b>{latest?.sequence || 0}</b>
        </span>
        <span>
          Command cursor <b>{run?.command_cursor || 0}</b>
        </span>
        <span>
          Run revision <b>{run?.revision || 0}</b>
        </span>
      </div>
      <div className="run-actions">
        <button
          disabled={terminal || !action?.enabled || cancelling}
          title={action?.reason || "Cancel this run"}
          onClick={onCancel}
        >
          cancel
        </button>
      </div>
    </section>
  );
}

function DecisionCard({
  session,
  id,
  canAnswer = true,
}: {
  session: string;
  id: string;
  canAnswer?: boolean;
}) {
  const q = useQuery({
    queryKey: ["decision", session, id],
    queryFn: () => api.decision(session, id),
  });
  const client = useQueryClient(),
    answer = useMutation({
      mutationFn: (option: string) =>
        api.answerDecision(session, id, q.data!.revision, option),
      onSuccess: () =>
        client.invalidateQueries({ queryKey: ["decision", session, id] }),
    });
  if (q.isLoading)
    return (
      <div className="card skeleton" role="status">
        Loading decision…
      </div>
    );
  if (q.error || !q.data) return null;
  return (
    <article className="card decision" data-state={q.data.state}>
      <header>
        <span>
          {q.data.state === "pending"
            ? "Decision required"
            : `Decision ${q.data.state}`}
        </span>
        <em>r{q.data.revision}</em>
      </header>
      <h3 id={`decision-${id}`}>{q.data.question}</h3>
      <div className="choices" role="group" aria-labelledby={`decision-${id}`}>
        {q.data.options.map((o) => (
          <button
            key={o.id}
            className={q.data!.selected_option_id === o.id ? "selected" : ""}
            aria-pressed={q.data!.selected_option_id === o.id}
            disabled={
              !canAnswer || q.data!.state !== "pending" || answer.isPending
            }
            title={
              canAnswer
                ? "Answer this revision"
                : "Capability manifest denies decision.answer"
            }
            onClick={() => answer.mutate(o.id)}
          >
            <strong>{o.label}</strong>
            <small>{o.description}</small>
          </button>
        ))}
      </div>
      {answer.error && (
        <p className="error" role="alert">
          {answer.error.message}
        </p>
      )}
    </article>
  );
}

function Workbench({ session }: { session?: Session }) {
  const capabilities = useQuery({
    queryKey: ["capabilities", session?.session_id],
    queryFn: () => api.capabilities(session!.session_id),
    enabled: !!session,
    refetchInterval: 5000,
  });
  const events = useQuery({
    queryKey: ["events", session?.session_id],
    queryFn: () => api.events(session!.session_id),
    enabled: !!session && capabilities.isSuccess,
    refetchInterval: 1500,
  });
  const sessionData = useQuery({
    queryKey: ["session-data", session?.session_id],
    queryFn: () => api.data(session!.session_id),
    enabled: !!session && capabilities.isSuccess,
    refetchInterval: 1500,
  });
  const interactions = useQuery({
    queryKey: ["interactions", session?.session_id],
    queryFn: () => api.interactions(session!.session_id),
    enabled: !!session && events.isSuccess,
    refetchInterval: 1500,
  });
  const nodes = interactions.data?.nodes || [];
  const latestRunId =
    [...(events.data?.items || [])].reverse().find((e) => e.run_id)?.run_id ||
    "";
  const latestTaskId =
    [...(events.data?.items || [])]
      .reverse()
      .map((e) => String(e.payload.task_id || ""))
      .find(Boolean) || "";
  const run = useQuery({
    queryKey: ["run", latestRunId],
    queryFn: () => api.run(latestRunId),
    enabled: !!latestRunId,
    refetchInterval: 1500,
  });
  const snapshot = useQuery({
    queryKey: ["snapshot", session?.session_id, latestTaskId, latestRunId],
    queryFn: () => api.snapshot(session!.session_id, latestTaskId, latestRunId),
    enabled: !!session && !!latestTaskId && !!latestRunId,
    refetchInterval: 1500,
  });
  const decisionIds = [
    ...new Set(
      nodes
        .filter((n) => n.kind === "decision")
        .map((n) => String(n.ref.decision_id || ""))
        .filter(Boolean),
    ),
  ];
  const [drawer, setDrawer] = useState("activity"),
    [input, setInput] = useState("");
  const timelineRef = useRef<HTMLDivElement>(null);
  const [followLatest, setFollowLatest] = useState(true);
  const client = useQueryClient();
  const runIsActive =
    !!run.data &&
    !["completed", "failed", "cancelled"].includes(run.data.state);
  const submit = useMutation({
    mutationFn: () =>
      runIsActive
        ? api.command(
            run.data!.run_id,
            session!.session_id,
            "steer",
            run.data!.revision,
            { input },
          )
        : api.startRun(session!.session_id, input),
    onSuccess: () => {
      setInput("");
      client.invalidateQueries({ queryKey: ["events", session?.session_id] });
      client.invalidateQueries({ queryKey: ["session-data", session?.session_id] });
      client.invalidateQueries({ queryKey: ["run", latestRunId] });
    },
  });
  const cancel = useMutation({
    mutationFn: () =>
      api.command(
        run.data!.run_id,
        session!.session_id,
        "cancel",
        run.data!.revision,
      ),
    onSuccess: () => {
      client.invalidateQueries({ queryKey: ["run", latestRunId] });
      client.invalidateQueries({ queryKey: ["events", session?.session_id] });
    },
  });
  const archive = useMutation({
    mutationFn: () =>
      api.transitionSession(
        session!.session_id,
        capabilities.data?.session_revision ?? session!.revision,
        "archived",
      ),
    onSuccess: () => client.invalidateQueries({ queryKey: ["sessions"] }),
  });
  const waitingDecision = nodes.some(
    (node) => node.kind === "decision" && node.state === "waiting",
  );
  const lastMessageId = sessionData.data?.messages.at(-1)?.message_id || "";
  useEffect(() => {
    if (drawer !== "activity" || !followLatest || waitingDecision) return;
    const frame = requestAnimationFrame(() => {
      const timeline = timelineRef.current;
      if (timeline) timeline.scrollTop = timeline.scrollHeight;
    });
    return () => cancelAnimationFrame(frame);
  }, [drawer, followLatest, lastMessageId, waitingDecision]);
  if (!session)
    return (
      <main className="empty">
        <div className="empty-orbit" aria-hidden="true">
          A
        </div>
        <h1>Select a Session</h1>
        <p>
          Conversation, planning, execution and evidence share one authoritative
          event cursor.
        </p>
      </main>
    );
  const startAction = capabilities.data?.actions.find(
    (a) => a.action_id === "run.start",
  );
  const steerAction = capabilities.data?.actions.find(
    (a) => a.action_id === "run.steer",
  );
  const decisionAction = capabilities.data?.actions.find(
    (a) => a.action_id === "decision.answer",
  );
  const sendEnabled = runIsActive ? steerAction?.enabled : startAction?.enabled;
  const drawerKinds: Record<string, string[]> = {
    activity: [],
    understanding: ["understanding"],
    plan: ["plan", "plan_node"],
    memory: ["memory_view"],
    files: ["artifact"],
    approval: ["approval"],
    evidence: ["evidence", "closure"],
  };
  const visibleNodes =
    drawer === "activity"
      ? nodes
      : nodes.filter((n) => drawerKinds[drawer]?.includes(n.kind));
  const commandError = submit.error || cancel.error;
  const transitionAction = capabilities.data?.actions.find(
    (a) => a.action_id === "session.transition",
  );
  return (
    <main className="workspace">
      <header className="topbar">
        <div>
          <small>{session.folder || "Workspace"} / Session</small>
          <h1>{session.title || session.session_id}</h1>
        </div>
        <div className="top-actions">
          <button
            disabled={
              !transitionAction?.enabled || archive.isPending || runIsActive
            }
            title={
              runIsActive
                ? "Cancel or complete the active Run before archiving"
                : transitionAction?.reason || "Archive Session"
            }
            onClick={() => archive.mutate()}
          >
            Archive
          </button>
          <div className="revision">
            Session r{capabilities.data?.session_revision ?? session.revision}
            <span>
              {snapshot.data
                ? `Task r${snapshot.data.task_revision} · Run r${snapshot.data.run_revision} · Projection r${snapshot.data.projection_revision}`
                : "authoritative"}
            </span>
          </div>
        </div>
      </header>
      <RunStrip
        events={events.data?.items || []}
        actions={capabilities.data?.actions || []}
        run={run.data}
        onCancel={() => cancel.mutate()}
        cancelling={cancel.isPending}
      />
      <div className="work-grid">
        <section className="conversation">
          <div className="context-tabs" role="tablist" aria-label="Task views">
            <button
              role="tab"
              aria-selected={drawer === "activity"}
              className={drawer === "activity" ? "active" : ""}
              onClick={() => setDrawer("activity")}
            >
              Conversation
            </button>
            <button
              role="tab"
              aria-selected={drawer === "understanding"}
              className={drawer === "understanding" ? "active" : ""}
              onClick={() => setDrawer("understanding")}
            >
              Understanding
            </button>
            <button
              role="tab"
              aria-selected={drawer === "plan"}
              className={drawer === "plan" ? "active" : ""}
              onClick={() => setDrawer("plan")}
            >
              Plan
            </button>
          </div>
          <div className="timeline" ref={timelineRef} aria-live="polite" onScroll={(event) => {
            const element = event.currentTarget;
            setFollowLatest(element.scrollHeight - element.scrollTop - element.clientHeight < 72);
          }}>
            {events.isError && (
              <div className="notice error" role="alert">
                <strong>Event stream unavailable</strong>
                <span>{events.error.message}</span>
              </div>
            )}
            {interactions.data?.stale && (
              <div className="notice error" role="status">
                <strong>Observation projection is catching up</strong>
                <span>
                  Event head {interactions.data.runtime_event_head}; projection
                  head {interactions.data.head_sequence}.
                </span>
              </div>
            )}
            {commandError && (
              <div className="notice error" role="alert">
                <strong>Run command conflict</strong>
                <span>
                  {commandError.message}; refresh uses the authoritative run
                  revision.
                </span>
              </div>
            )}
            {sessionData.isError && (
              <div className="notice error" role="alert">
                <strong>Conversation history unavailable</strong>
                <span>{sessionData.error.message}</span>
              </div>
            )}
            {archive.error && (
              <div className="notice error" role="alert">
                <strong>Session transition failed</strong>
                <span>{archive.error.message}</span>
              </div>
            )}
            {nodes.length === 0 && !interactions.isLoading && (
              <div className="welcome">
                <span aria-hidden="true">◈</span>
                <h2>Ready for a governed task</h2>
                <p>
                  Semantic decisions, plans, tool observations and closure
                  evidence appear here as durable facts.
                </p>
              </div>
            )}
            {decisionIds.map((id) => (
              <DecisionCard
                key={id}
                session={session.session_id}
                id={id}
                canAnswer={!!decisionAction?.enabled}
              />
            ))}
            {drawer === "activity" && (sessionData.data?.messages || []).map((message) => (
              <article
                className={`message ${message.role}`}
                key={message.message_id}
                data-turn-id={message.turn_id}
              >
                <header>
                  <strong>{message.role === "user" ? "You" : "Agent"}</strong>
                  <time dateTime={message.created_at}>{displayTime(message.created_at)}</time>
                </header>
                {message.role === "assistant" ? (
                  <MarkdownContent content={message.content} />
                ) : (
                  <div className="plain-message">{message.content}</div>
                )}
              </article>
            ))}
            {drawer !== "activity" && visibleNodes.slice(-20).map((node) => (
              <article className={`event ${node.kind}`} key={node.node_id}>
                <span className="event-icon" aria-hidden="true">
                  {glyph[node.kind] || "·"}
                </span>
                <div>
                  <header>
                    <strong>{node.label}</strong>
                    <time>{node.state}</time>
                  </header>
                  <p>
                    {node.summary ||
                      `Durable event from run ${String(node.ref.run_id || "")}`}
                  </p>
                </div>
              </article>
            ))}
          </div>
          {!followLatest && drawer === "activity" && (
            <button className="timeline-jump" onClick={() => {
              const timeline = timelineRef.current;
              if (timeline) timeline.scrollTo({ top: timeline.scrollHeight, behavior: "smooth" });
              setFollowLatest(true);
            }}>Jump to latest</button>
          )}
          <form
            className="composer"
            onSubmit={(e) => {
              e.preventDefault();
              if (input.trim() && sendEnabled) submit.mutate();
            }}
          >
            <textarea
              aria-label="Message"
              value={input}
              onChange={(e) => setInput(e.target.value)}
              placeholder={
                runIsActive
                  ? "Add information or steer the active run…"
                  : "Describe a governed task…"
              }
            />
            <footer>
              <span>
                {runIsActive
                  ? `Steer will require Run r${run.data!.revision}`
                  : "Start a new governed Run"}
              </span>
              <button
                disabled={!sendEnabled || !input.trim() || submit.isPending}
                title={
                  (runIsActive ? steerAction : startAction)?.reason ||
                  "Submit a governed command"
                }
              >
                {runIsActive ? "Steer" : "Start"}
              </button>
            </footer>
          </form>
        </section>
        <aside className="drawer">
          <nav role="tablist" aria-label="Evidence views">
            {[
              "activity",
              "understanding",
              "plan",
              "memory",
              "files",
              "approval",
              "evidence",
            ].map((x) => (
              <button
                role="tab"
                aria-selected={drawer === x}
                key={x}
                className={drawer === x ? "active" : ""}
                onClick={() => setDrawer(x)}
              >
                {x}
              </button>
            ))}
          </nav>
          <div className="drawer-body" role="tabpanel" tabIndex={0}>
            <h2>{drawer[0].toUpperCase() + drawer.slice(1)}</h2>
            <p className="muted">
              Event head{" "}
              {interactions.data?.runtime_event_head || events.data?.head || 0}{" "}
              · Projection head {interactions.data?.head_sequence || 0}
            </p>
            {visibleNodes.slice(-12).map((n) => (
              <div className="mini" key={n.node_id}>
                <span aria-hidden="true">{glyph[n.kind] || "·"}</span>
                <div>
                  <strong>{n.label}</strong>
                  <small>{n.summary || n.state}</small>
                  {n.summary && <small>{n.state}</small>}
                </div>
              </div>
            ))}
            {visibleNodes.length === 0 && (
              <p className="muted">
                No durable {drawer} facts have been published for this Session.
              </p>
            )}
          </div>
        </aside>
      </div>
    </main>
  );
}

export function App() {
  const [search, setSearch] = useState(""),
    [selected, setSelected] = useState(() => location.hash.slice(1));
  const [locale, setLocale] = useState<Locale>(initialLocale);
  const [railOpen, setRailOpen] = useState(() => new URLSearchParams(location.search).get("rail") === "open");
  const [runtimePanel, setRuntimePanel] = useState<"profile" | "settings" | null>(() => {
    const panel = new URLSearchParams(location.search).get("panel");
    return panel === "profile" || panel === "settings" ? panel : null;
  });
  const t = (key: Parameters<typeof translate>[1]) => translate(locale, key);
  const sessions = useQuery({
    queryKey: ["sessions", search],
    queryFn: () => api.sessions(search),
    refetchInterval: 10000,
  });
  useEffect(() => {
    if (
      sessions.data &&
      !sessions.data.items.some((item) => item.session_id === selected)
    )
      setSelected(sessions.data.items[0]?.session_id || "");
  }, [sessions.data, selected]);
  useEffect(() => {
    if (selected) history.replaceState(null, "", `${location.pathname}${location.search}#${selected}`);
  }, [selected]);
  useEffect(() => {
    localStorage.setItem("af.locale", locale);
    document.documentElement.lang = locale;
  }, [locale]);
  const active = sessions.data?.items.find((s) => s.session_id === selected);
  const selectSession = (id: string) => {
    setSelected(id);
    setRailOpen(false);
  };
  const openRuntimePanel = (panel: "profile" | "settings" | null) => {
    setRuntimePanel(panel);
    const url = new URL(location.href);
    if (panel) url.searchParams.set("panel", panel);
    else url.searchParams.delete("panel");
    history.replaceState(null, "", `${url.pathname}${url.search}${location.hash}`);
  };
  return (
    <div className="app">
      <a className="skip-link" href="#main-workbench">
        Skip to workbench
      </a>
      <SessionRail
        items={sessions.data?.items || []}
        active={selected}
        search={search}
        onSearch={setSearch}
        onSelect={selectSession}
        onOpenPanel={(panel) => openRuntimePanel(panel)}
        open={railOpen}
        locale={locale}
      />
      {railOpen && (
        <button
          className="rail-scrim"
          aria-label={t("closeSessions")}
          onClick={() => setRailOpen(false)}
        />
      )}
      <div id="main-workbench" className="main-host">
        {sessions.isError ? (
          <main className="empty">
            <div className="notice error" role="alert">
              <strong>Cannot load Sessions</strong>
              <span>{sessions.error.message}</span>
            </div>
            <h1>Runtime connection required</h1>
            <p>Configure the authenticated `/api/v1` endpoint, then retry.</p>
            <button onClick={() => sessions.refetch()}>Retry</button>
          </main>
        ) : (
          <Workbench session={active} />
        )}
      </div>
      <div className="locale-switcher">
        <label>
          {t("locale")}
          <select
            value={locale}
            onChange={(event) => setLocale(event.target.value as Locale)}
          >
            <option value="en">{t("english")}</option>
            <option value="zh-CN">{t("chinese")}</option>
          </select>
        </label>
      </div>
      <div className="mobile-switcher">
        <button
          aria-controls="session-rail"
          aria-expanded={railOpen}
          aria-label={railOpen ? t("closeSessions") : t("openSessions")}
          onClick={() => setRailOpen((value) => !value)}
        >
          ☰ {t("sessions")}
        </button>
        <span>{active?.title || `Agent ${t("workbench")}`}</span>
      </div>
      {runtimePanel && <RuntimePanel mode={runtimePanel} onClose={() => openRuntimePanel(null)} onLanguage={setLocale} />}
    </div>
  );
}
