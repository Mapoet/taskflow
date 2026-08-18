import { expect, test } from "@playwright/test";

const connected = Boolean(process.env.AF_WORKBENCH_BASE_URL);
const evidenceDirectory = process.env.AF_WORKBENCH_SCREENSHOT_DIR;
test.skip(!connected, "requires the connected SQLite Workbench runtime");

for (const viewport of [
  { name: "desktop", width: 1440, height: 1000 },
  { name: "tablet", width: 820, height: 1180 },
  { name: "mobile-zh", width: 390, height: 844 },
]) {
  test(`captures ${viewport.name} Workbench evidence`, async ({
    page,
  }, testInfo) => {
    await page.setViewportSize({
      width: viewport.width,
      height: viewport.height,
    });
    await page.goto("/#session-orbital");
    await expect(
      page.getByRole("heading", {
        name: "Orbital analysis and runtime closure",
      }),
    ).toBeVisible();
    if (viewport.name === "mobile-zh") {
      await page.getByLabel("Language").selectOption("zh-CN");
      await page.getByRole("button", { name: "打开会话列表" }).click();
      await expect(
        page.getByRole("complementary", { name: "会话" }),
      ).toBeVisible();
      await page.waitForTimeout(300);
    }
    expect(
      await page
        .locator("body")
        .evaluate((body) => body.scrollWidth <= body.clientWidth),
    ).toBe(true);
    const screenshotPath = evidenceDirectory
      ? `${evidenceDirectory}/af-tgui-${viewport.name}.png`
      : testInfo.outputPath(`af-tgui-${viewport.name}.png`);
    await page.screenshot({
      path: screenshotPath,
      fullPage: true,
    });
  });
}
