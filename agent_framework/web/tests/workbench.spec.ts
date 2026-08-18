import { test, expect } from "@playwright/test";
const connected = !!process.env.AF_WORKBENCH_BASE_URL;
if (!connected)
  test("renders a truthful disconnected state when production API is unavailable", async ({
    page,
  }) => {
    await page.goto("/");
    await expect(
      page.getByText(/Runtime connection required|Select a Session/),
    ).toBeVisible();
    await expect(page.locator("body")).not.toContainText(
      "task_completion_verified",
    );
  });

if (connected)
  test("renders durable runtime facts and completes an accountable decision", async ({
    page,
  }) => {
    await page.goto("/#session-orbital");
    await expect(
      page.getByText("Orbital analysis and runtime closure").first(),
    ).toBeVisible();
    await expect(page.locator("body")).not.toContainText(
      "ISOLATED MEMORY CANARY",
    );
    await expect(page.locator("body")).not.toContainText(
      "ISOLATED TOOL CANARY",
    );
    await expect(page.locator("body")).not.toContainText(
      "ISOLATED ARTIFACT CANARY",
    );
    await expect(
      page.getByText("How deeply should the orbital workflow be verified?"),
    ).toBeVisible();
    await expect(
      page.getByText(
        "Long-running read-only analysis · professional assurance",
      ),
    ).toBeVisible();
    await page.getByRole("button", { name: /Professional assurance/ }).click();
    await expect(
      page.getByRole("button", { name: /Professional assurance/ }),
    ).toBeDisabled();
    await page.getByRole("tab", { name: "Plan", exact: true }).click();
    await expect(
      page.getByRole("heading", { name: "Plan", exact: true }),
    ).toBeVisible();
    await expect(
      page.getByText(/Event head \d+ · Projection head \d+/),
    ).toBeVisible();
    await expect(page.locator("body")).not.toContainText(
      "task_completion_verified",
    );
    await expect(page.locator("body")).not.toContainText(
      "model_turn_cannot_verify_task",
    );
  });
