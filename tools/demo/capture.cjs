// Captures the README's demo images from a running replay of a SYNTHETIC
// recording. Never point it at a real OpenSky recording: published images
// must not show real aircraft identifiers (see README, Etik ve hukuki sınırlar).
//
//   ./build/release/sema_replay tests/fixtures/anomaly_synthetic.jsonl --loss-min-coverage 3 \
//       --serve 8080 --speed 30 --label "sentetik veri" --identifiers full   # synthetic ids only
//   node tools/demo/capture.cjs [http://127.0.0.1:8080/] [docs/img]
//
// Needs Playwright (`npm i -D playwright && npx playwright install chromium`)
// and ffmpeg for the GIF. The "sentetik veri" replay banner and the
// "Veri: OpenSky Network" attribution stay visible in every frame on purpose.
// If the base map cannot be loaded (offline), its notice is hidden and the
// images show the boundary and aircraft only.

const { chromium } = require("playwright");
const { execFileSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const url = process.argv[2] || "http://127.0.0.1:8080/";
const out = path.resolve(process.argv[3] || "docs/img");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function settle(page) {
  // Offline: hide the "base map failed" notice, keep the replay banner.
  await page.evaluate(() => {
    const b = document.getElementById("banner-style");
    if (b) b.style.display = "none";
  });
  await sleep(400);
}

async function main() {
  fs.mkdirSync(out, { recursive: true });
  const browser = await chromium.launch();

  // Desktop: live map, then a GIF of a few seconds of movement.
  const page = await browser.newPage({ viewport: { width: 1280, height: 760 }, deviceScaleFactor: 1 });
  await page.goto(url);
  await page.waitForFunction(() => /\d+ uçak/.test(document.getElementById("st-count")?.textContent || ""));
  await sleep(4000);
  if (!(await page.textContent("#replay-label")).includes("sentetik")) {
    throw new Error('replay label must say "sentetik veri"; capture only synthetic recordings');
  }
  await settle(page);
  await page.screenshot({ path: path.join(out, "harita.png") });

  const frames = fs.mkdtempSync(path.join(os.tmpdir(), "sema-frames-"));
  for (let i = 0; i < 40; i++) {
    await settle(page);
    await page.screenshot({ path: path.join(frames, `f${String(i).padStart(3, "0")}.png`) });
    await sleep(200);
  }
  execFileSync("ffmpeg", [
    "-loglevel", "error", "-y", "-framerate", "8", "-i", path.join(frames, "f%03d.png"),
    "-vf", "scale=960:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96[p];[b][p]paletteuse=dither=bayer",
    path.join(out, "demo.gif"),
  ]);
  fs.rmSync(frames, { recursive: true, force: true });

  // Events: list, then the first event's detail with its track on the map.
  await page.click("#btn-events");
  await page.waitForSelector(".ev-item");
  await settle(page);
  await page.screenshot({ path: path.join(out, "olaylar.png") });
  await page.click(".ev-item");
  await sleep(1500);
  await settle(page);
  await page.screenshot({ path: path.join(out, "olay-ayrinti.png") });
  await page.click("#events-close");

  // Statistics, from the default view (the event detail zoomed in).
  await page.goto(url);
  await page.waitForFunction(() => /\d+ uçak/.test(document.getElementById("st-count")?.textContent || ""));
  await page.click("#mode-stats");
  await page.waitForFunction(() => (document.getElementById("k-flights")?.textContent || "—") !== "—");
  await sleep(1500);
  await settle(page);
  await page.screenshot({ path: path.join(out, "istatistik.png") });

  // Phone.
  const phone = await browser.newPage({ viewport: { width: 390, height: 780 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
  await phone.goto(url);
  await phone.waitForFunction(() => /\d+ uçak/.test(document.getElementById("st-count")?.textContent || ""));
  await sleep(3000);
  await settle(phone);
  await phone.screenshot({ path: path.join(out, "telefon.png") });

  await browser.close();
  console.log(`wrote ${fs.readdirSync(out).join(", ")} to ${out}`);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
