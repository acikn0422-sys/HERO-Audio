"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const path = require("node:path");
const {pathToFileURL} = require("node:url");
const {chromium} = require(process.env.HERO_PLAYWRIGHT_MODULE || "playwright");
const {makeWav} = require("./fixtures.cjs");

(async () => {
  const root = path.resolve(__dirname, "../../.."), base = path.join(root, "build/annotator-browser");
  await fs.mkdir(base, {recursive: true});
  const output = await fs.mkdtemp(path.join(base, "run-"));
  const browser = await chromium.launch({headless: true, executablePath: process.env.HERO_BROWSER_EXECUTABLE || undefined,
    args: ["--mute-audio"], downloadsPath: output});
  try {
    const context = await browser.newContext({viewport: {width: 1440, height: 1050}, acceptDownloads: true});
    const network = [], errors = [];
    await context.route(/^https?:/, route => { network.push(route.request().url()); return route.abort(); });
    const page = await context.newPage(); page.on("pageerror", error => errors.push(error.message));
    page.on("console", msg => { if (msg.type() === "error") errors.push(msg.text()); });
    page.on("dialog", dialog => dialog.accept());
    await page.goto(pathToFileURL(path.join(root, "tools/audio-annotator/index.html")).href);
    await page.locator("#audio-file").setInputFiles({name: "synthetic.wav", mimeType: "audio/wav", buffer: makeWav()});
    await page.waitForFunction(() => document.getElementById("status").textContent.includes("录音已载入"));
    assert.match(await page.locator("#audio-info").textContent(), /44,100 Hz/);
    await page.locator("#baseline").fill("1"); await page.locator("#baseline").blur();
    await page.locator("#start-time").fill("2"); await page.locator("#end-time").fill("2.1");
    await page.locator("#annotator").fill("browser-tester"); await page.locator("#save-label").click();
    assert.equal(await page.locator("#labels-body tr").count(), 1);
    const start = await page.locator("#start-time").inputValue();
    const end = await page.locator("#end-time").inputValue();
    await page.locator("#fit-selection").click(); await page.locator("#zoom-in").click(); await page.locator("#zoom-out").click();
    assert.equal(await page.locator("#start-time").inputValue(), start);
    assert.equal(await page.locator("#end-time").inputValue(), end);

    // Real Web Audio paths, including rate-independent source timestamps and looping.
    await page.locator("#play-selection").click();
    await page.waitForFunction(() => document.getElementById("play-state").textContent.includes("播放"));
    await page.waitForFunction(() => document.getElementById("play-state").textContent === "已停止");
    await page.locator("#loop").check(); await page.locator("#rate").selectOption("0.5");
    await page.locator("#play-selection").click();
    await page.waitForFunction(() => document.getElementById("play-state").textContent.includes("循环 0.5×"));
    await page.locator("#stop").click();
    assert.equal(await page.locator("#start-time").inputValue(), start);
    await page.locator("#loop").uncheck();

    await page.locator("#export-csv").click();
    assert.match(await page.locator("#error").textContent(), /确认/);
    await page.locator("#baseline-confirmed").check();
    async function download(id, filename) {
      const ready = page.waitForEvent("download"); await page.locator(`#${id}`).click();
      const item = await ready; await item.saveAs(path.join(output, filename));
      return fs.readFile(path.join(output, filename), "utf8");
    }
    const csv = await download("export-csv", "export.csv");
    assert.equal(csv.split("\n")[1], "2,2.1,anomaly_impact,steady,medium,browser-tester");
    const draft = JSON.parse(await download("save-draft", "draft.json"));
    assert.equal(draft.labels[0].startSample, 88200);
    assert.equal(draft.labels[0].endSample, 92610);
    assert.equal(draft.source.sha256.length, 64);

    await page.getByRole("button", {name: "删除标签 1", exact: true}).click();
    assert.equal(await page.locator("#labels-body tr").count(), 0);
    await page.locator("#undo").click(); assert.equal(await page.locator("#labels-body tr").count(), 1);
    await page.locator("#redo").click(); assert.equal(await page.locator("#labels-body tr").count(), 0);
    await page.locator("#draft-file").setInputFiles({name: "draft.json", mimeType: "application/json", buffer: Buffer.from(JSON.stringify(draft))});
    await page.waitForFunction(() => document.querySelectorAll("#labels-body tr").length === 1);
    const wrong = {...draft, source: {...draft.source, sha256: "0".repeat(64)}};
    await page.locator("#draft-file").setInputFiles({name: "wrong.json", mimeType: "application/json", buffer: Buffer.from(JSON.stringify(wrong))});
    await page.waitForFunction(() => document.getElementById("error").textContent.includes("指纹"));
    assert.equal(await page.locator("#labels-body tr").count(), 1);

    await page.locator("#new-label").click();
    await page.locator("#start-time").fill("2.05"); await page.locator("#end-time").fill("2.2");
    await page.locator("#save-label").click(); assert.match(await page.locator("#error").textContent(), /重叠/);
    assert.equal(await page.locator("#labels-body tr").count(), 1);
    await page.getByRole("button", {name: "编辑标签 1", exact: true}).click();
    await page.locator("#end-time").fill("2.12"); await page.locator("#save-label").click();
    assert.match(await page.locator("#labels-body").textContent(), /2\.120000/);
    await page.locator("#undo").click(); assert.match(await page.locator("#labels-body").textContent(), /2\.100000/);

    // Pointer selection uses the same original sample clock as numeric input.
    await page.locator("#new-label").click(); await page.locator("#fit-all").click();
    const rect = await page.locator("#detail").boundingBox();
    const px = seconds => rect.x + 62 + seconds / 4 * (rect.width - 80);
    await page.mouse.move(px(2.4), rect.y + 100); await page.mouse.down();
    await page.mouse.move(px(2.6), rect.y + 100, {steps: 8}); await page.mouse.up();
    assert.ok(Math.abs(Number(await page.locator("#start-time").inputValue()) - 2.4) < 0.003);
    assert.ok(Math.abs(Number(await page.locator("#end-time").inputValue()) - 2.6) < 0.003);
    await page.screenshot({path: path.join(output, "desktop.png"), fullPage: true});
    await page.setViewportSize({width: 390, height: 844});
    await page.screenshot({path: path.join(output, "mobile.png"), fullPage: true});
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth));
    assert.deepEqual(network, []); assert.deepEqual(errors, []);
    console.log(`PASS browser interaction, audio playback, export, restore, errors, pointer coordinates, responsive layout, no network\nArtifacts: ${output}`);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
