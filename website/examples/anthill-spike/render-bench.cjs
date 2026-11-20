// Times the CPU renderer's per-frame cost in Chromium: interpolate n ants
// from (x0,y0,t0)->(x1,y1,t1) typed arrays into an ImageData and put it on a
// 1024x768 canvas. node render-bench.cjs 1000000
const { chromium } = require('playwright');
(async () => {
  const n = Number(process.argv[2] ?? 1000000);
  const browser = await chromium.launch();
  const page = await browser.newPage();
  await page.setContent('<canvas id=c width=1024 height=768></canvas>');
  const r = await page.evaluate((n) => {
    const W = 1024, H = 768, ctx = document.getElementById('c').getContext('2d');
    const img = ctx.createImageData(W, H), px = new Uint32Array(img.data.buffer);
    const x0 = new Float32Array(n), y0 = new Float32Array(n), x1 = new Float32Array(n), y1 = new Float32Array(n);
    const t0 = new Float32Array(n), inv = new Float32Array(n), col = new Uint32Array(n);
    for (let i = 0; i < n; i++) { x0[i] = Math.random() * W; y0[i] = Math.random() * H; x1[i] = Math.random() * W; y1[i] = Math.random() * H; t0[i] = 0; inv[i] = 1 / (2000 + Math.random() * 6000); col[i] = i & 1 ? 0xff3060e0 : 0xff20a0f0; }
    const times = [];
    for (let f = 0; f < 60; f++) {
      const a = performance.now(), now = f * 16;
      px.fill(0xff203040);
      for (let i = 0; i < n; i++) {
        let u = (now - t0[i]) * inv[i]; u = u < 0 ? 0 : u > 1 ? 1 : u;
        const x = (x0[i] + (x1[i] - x0[i]) * u) | 0, y = (y0[i] + (y1[i] - y0[i]) * u) | 0;
        px[y * W + x] = col[i];
      }
      ctx.putImageData(img, 0, 0);
      times.push(performance.now() - a);
    }
    times.sort((a, b) => a - b);
    return { n, median_ms: +times[30].toFixed(2), p90_ms: +times[54].toFixed(2) };
  }, n);
  console.log(JSON.stringify({ browser: 'chromium ' + browser.version(), what: 'CPU interpolate + putImageData 1024x768', ...r }));
  await browser.close();
})();
