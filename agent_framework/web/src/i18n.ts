export type Locale = "en" | "zh-CN";

const messages = {
  en: {
    workbench: "Workbench",
    sessions: "Sessions",
    search: "Search sessions",
    connected: "Runtime connected",
    locale: "Language",
    english: "English",
    chinese: "简体中文",
    closeSessions: "Close Sessions",
    openSessions: "Open Sessions",
  },
  "zh-CN": {
    workbench: "工作台",
    sessions: "会话",
    search: "搜索会话",
    connected: "运行时已连接",
    locale: "语言",
    english: "English",
    chinese: "简体中文",
    closeSessions: "关闭会话列表",
    openSessions: "打开会话列表",
  },
} as const;

export type MessageKey = keyof typeof messages.en;
export const translate = (locale: Locale, key: MessageKey): string =>
  messages[locale][key];
export function initialLocale(): Locale {
  const saved = localStorage.getItem("af.locale");
  if (saved === "en" || saved === "zh-CN") return saved;
  return navigator.language.toLowerCase().startsWith("zh") ? "zh-CN" : "en";
}
