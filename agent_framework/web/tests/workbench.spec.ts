import{test,expect}from'@playwright/test';
test('renders a truthful disconnected state when production API is unavailable',async({page})=>{await page.goto('/');await expect(page.getByText(/Runtime connection required|Select a Session/)).toBeVisible();await expect(page.locator('body')).not.toContainText('task_completion_verified')});
