import { useEffect, useMemo, useState } from "react";
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import { api } from "./api";
import type { RuntimeSettingField } from "./types";

export function RuntimePanel({
  mode,
  onClose,
  onLanguage,
}: {
  mode: "profile" | "settings";
  onClose: () => void;
  onLanguage: (value: "en" | "zh-CN") => void;
}) {
  const client = useQueryClient();
  const profile = useQuery({ queryKey: ["runtime-profile"], queryFn: api.profile });
  const settings = useQuery({
    queryKey: ["runtime-settings"],
    queryFn: api.settings,
    enabled: mode === "settings",
  });
  const [draft, setDraft] = useState<Record<string, string | boolean>>({});
  useEffect(() => {
    if (!settings.data) return;
    setDraft(
      Object.fromEntries(
        settings.data.fields
          .filter((field) => field.mutability !== "read_only")
          .map((field) => [field.key, field.value ?? ""]),
      ),
    );
  }, [settings.data]);
  const changed = useMemo(() => {
    const updates: Record<string, string | boolean> = {};
    for (const field of settings.data?.fields || []) {
      if (field.mutability === "read_only") continue;
      if (draft[field.key] !== field.value) updates[field.key] = draft[field.key];
    }
    return updates;
  }, [draft, settings.data]);
  const save = useMutation({
    mutationFn: () => api.updateSettings(settings.data!.revision, changed),
    onSuccess: async () => {
      if (typeof changed["appearance.language"] === "string")
        onLanguage(changed["appearance.language"] as "en" | "zh-CN");
      if (typeof changed["provider.id"] === "string")
        localStorage.setItem("af.provider", changed["provider.id"]);
      await client.invalidateQueries({ queryKey: ["runtime-settings"] });
    },
  });
  const categories = useMemo(() => {
    const grouped = new Map<string, RuntimeSettingField[]>();
    for (const field of settings.data?.fields || []) {
      const values = grouped.get(field.category) || [];
      values.push(field);
      grouped.set(field.category, values);
    }
    return [...grouped.entries()];
  }, [settings.data]);
  return (
    <div className="panel-scrim" role="presentation" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <section className="runtime-panel" role="dialog" aria-modal="true" aria-labelledby="runtime-panel-title">
        <header>
          <div>
            <small>{mode === "profile" ? "Authenticated runtime identity" : "Revision-aware deployment configuration"}</small>
            <h2 id="runtime-panel-title">{mode === "profile" ? "User and scope" : "System settings"}</h2>
          </div>
          <button onClick={onClose} aria-label="Close panel">Close</button>
        </header>
        <div className="runtime-panel-body">
          {profile.isError && <p className="error" role="alert">{profile.error.message}</p>}
          {mode === "profile" && profile.data && (
            <dl className="profile-grid">
              {[
                ["Principal", profile.data.principal_id], ["Tenant", profile.data.tenant_id],
                ["Organization", profile.data.organization_id], ["Project", profile.data.project_id],
                ["Workspace", profile.data.workspace_id], ["Agent", profile.data.agent_id],
                ["Authentication", profile.data.authenticated ? "Authenticated" : "Unavailable"],
                ["Authorization revision", `r${profile.data.authorization_revision}`],
              ].map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{value || "Not configured"}</dd></div>)}
            </dl>
          )}
          {mode === "settings" && settings.isLoading && <p role="status">Loading authoritative settings…</p>}
          {mode === "settings" && settings.isError && <p className="error" role="alert">{settings.error.message}</p>}
          {mode === "settings" && settings.data && (
            <form onSubmit={(event) => { event.preventDefault(); if (Object.keys(changed).length) save.mutate(); }}>
              <div className="settings-meta">Settings r{settings.data.revision} · authorization r{settings.data.authorization_revision}</div>
              {categories.map(([category, fields]) => (
                <fieldset key={category}>
                  <legend>{category}</legend>
                  {fields.map((field) => <SettingControl key={field.key} field={field} value={draft[field.key]} onChange={(value) => setDraft((old) => ({ ...old, [field.key]: value }))} />)}
                </fieldset>
              ))}
              {save.error && <p className="error" role="alert">{save.error.message}</p>}
              {save.isSuccess && <p className="settings-success" role="status">Saved as revision {save.data.revision}. {save.data.restart_required ? "Restart required before deployment fields become active." : "Dynamic preferences are active."}</p>}
              <footer><button type="button" onClick={onClose}>Cancel</button><button className="primary" disabled={save.isPending || Object.keys(changed).length === 0}>Save changes</button></footer>
            </form>
          )}
        </div>
      </section>
    </div>
  );
}

function SettingControl({ field, value, onChange }: { field: RuntimeSettingField; value: string | boolean | undefined; onChange: (value: string | boolean) => void }) {
  const readOnly = field.mutability === "read_only";
  return (
    <label className="setting-row">
      <span><strong>{field.label}</strong><small>{field.key} · {field.mutability.replace("_", " ")}</small></span>
      {field.type === "boolean" ? (
        <input type="checkbox" checked={Boolean(readOnly ? field.value : value)} disabled={readOnly} onChange={(e) => onChange(e.target.checked)} />
      ) : field.type === "select" ? (
        <select value={String(value ?? field.value ?? "")} disabled={readOnly} onChange={(e) => onChange(e.target.value)}>{field.options?.map((option) => <option key={option}>{option}</option>)}</select>
      ) : (
        <input type="text" value={String(readOnly ? field.value ?? "Not configured" : value ?? "")} disabled={readOnly} spellCheck={false} onChange={(e) => onChange(e.target.value)} />
      )}
    </label>
  );
}
