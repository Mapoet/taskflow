import { defineConfig, devices } from "@playwright/test";
const runtime = process.env.AF_WORKBENCH_BASE_URL;
export default defineConfig({
  testDir: "./tests",
  workers: runtime ? 1 : undefined,
  timeout: 90_000,
  expect: { timeout: 15_000 },
  use: {
    baseURL: runtime || "http://127.0.0.1:4173",
    trace: runtime ? "off" : "retain-on-failure",
    screenshot: "only-on-failure",
    locale: "en-US",
  },
  webServer: runtime
    ? undefined
    : {
        command: "npm run dev",
        url: "http://127.0.0.1:4173",
        reuseExistingServer: true,
      },
  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
    { name: "firefox", use: { ...devices["Desktop Firefox"] } },
    { name: "webkit", use: { ...devices["Desktop Safari"] } },
  ],
});
