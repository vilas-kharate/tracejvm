// Runs the Shiklo Java checklist (java-checklist.html: one program per row of the full-language checklist, each
// compared with the exact output of a real JDK) against the runtime in ./vendor/runtime, in Playwright's Chromium.
// Prints the page's report; exits 0 even when rows fail (the report is the result), 2 when the page did not finish.
//   node shiklo/checklist/run.mjs [timeoutSeconds]
import { createReadStream, statSync } from "node:fs";
import { createServer } from "node:http";
import { extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

const root = fileURLToPath(new URL(".", import.meta.url));
const timeoutMs = Number(process.argv[2] ?? 900) * 1000;
const types = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".mjs": "text/javascript; charset=utf-8",
  ".json": "application/json", ".wasm": "application/wasm", ".jar": "application/java-archive",
};

// Cross-origin isolated (TraceJVM's host bridge uses SharedArrayBuffer) and answering requests in parallel (TraceJVM's
// worker fetches several files together and reads none until all have answered).
const server = createServer((req, res) => {
  const path = normalize(decodeURIComponent(new URL(req.url, "http://x").pathname)).replace(/^([/\\])+/, "");
  const file = join(root, path);
  res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
  res.setHeader("Cross-Origin-Resource-Policy", "same-origin");
  res.setHeader("Cache-Control", "no-store");
  let size;
  try {
    if (!file.startsWith(root)) throw new Error("outside");
    size = statSync(file).size;
  } catch {
    res.statusCode = 404;
    res.end();
    return;
  }
  res.setHeader("Content-Type", types[extname(file)] ?? "application/octet-stream");
  res.setHeader("Content-Length", size);
  createReadStream(file).pipe(res);
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const url = `http://127.0.0.1:${server.address().port}/java-checklist.html?coi=1&rt=runtime`;

const browser = await chromium.launch();
let code = 0;
try {
  const page = await browser.newPage();
  page.on("pageerror", (error) => console.error(`page error: ${error.message}`));
  await page.goto(url);
  try {
    await page.waitForFunction(() => /\nDONE\n?$|^DONE\n?$/.test(document.getElementById("out")?.textContent ?? ""), null,
      { timeout: timeoutMs, polling: 1000 });
  } catch {
    code = 2;
  }
  const report = await page.evaluate(() => document.getElementById("out")?.textContent ?? "");
  process.stdout.write(report.endsWith("\n") ? report : `${report}\n`);
  if (code) process.stdout.write(`TIMEOUT: the page did not finish within ${timeoutMs / 1000} s\n`);
} finally {
  await browser.close();
  server.close();
}
process.exit(code);
