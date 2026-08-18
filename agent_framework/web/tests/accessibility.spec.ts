import AxeBuilder from "@axe-core/playwright";
import { expect, test } from "@playwright/test";

const connected = Boolean(process.env.AF_WORKBENCH_BASE_URL);

test.describe("Workbench accessibility and localization", () => {
  test.skip(!connected, "requires the connected SQLite Workbench runtime");

  test("has no serious or critical axe findings and exposes keyboard-focusable decisions", async ({
    page,
  }) => {
    await page.goto("/#session-orbital");
    await expect(
      page.getByRole("heading", {
        name: "Orbital analysis and runtime closure",
      }),
    ).toBeVisible();
    const results = await new AxeBuilder({ page }).analyze();
    const blockers = results.violations.filter((result) =>
      ["serious", "critical"].includes(result.impact || ""),
    );
    expect(blockers, JSON.stringify(blockers, null, 2)).toEqual([]);

    const professional = page.getByRole("button", {
      name: /Professional assurance/,
    });
    await professional.focus();
    await expect(professional).toBeFocused();
  });

  test("switches document language and preserves bilingual layout", async ({
    page,
  }) => {
    await page.goto("/#session-orbital");
    await page.getByLabel("Language").selectOption("zh-CN");
    await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
    await expect(
      page.getByRole("complementary", { name: "会话" }),
    ).toBeVisible();
    await expect(page.getByText("运行时已连接")).toBeVisible();
    await page.reload();
    await expect(page.locator("html")).toHaveAttribute("lang", "zh-CN");
    expect(
      await page
        .locator("body")
        .evaluate((body) => body.scrollWidth <= body.clientWidth),
    ).toBe(true);
  });
});
