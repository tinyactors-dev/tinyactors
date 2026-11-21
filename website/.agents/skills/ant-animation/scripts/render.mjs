// Renders stage.html (the motion study) frame by frame with Playwright.
//   node render.mjs stills 1,5,9 [outdir]   PNGs at those times (ascending: the scene is simulated forward)
//   node render.mjs video [fps] [outdir]    ant-motion.mp4, frame-exact, 1920x1080 (needs ffmpeg)
// The page is served from this directory; /ds/ is the design system
// (.agents/skills/tinyactors-design at the repository root), so the ant,
// the colour tokens and the fonts are read from their one copy.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
const { chromium } = createRequire(import.meta.url)('playwright');

const dir = path.dirname(new URL(import.meta.url).pathname);
const ds = path.resolve(dir, '../../../../../.agents/skills/tinyactors-design');
const types = { '.html': 'text/html', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml', '.woff2': 'font/woff2' };
const server = http.createServer((req, res) => {
  const url = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const [root, rest] = url.startsWith('/ds/') ? [ds, url.slice(3)] : [dir, url];
  const file = path.join(root, rest);
  if (!file.startsWith(root) || !fs.existsSync(file) || !fs.statSync(file).isFile()) { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'content-type': types[path.extname(file)] ?? 'application/octet-stream' });
  fs.createReadStream(file).pipe(res);
}).listen(0);
const port = server.address().port;

const [mode = 'stills', arg, outArg] = process.argv.slice(2);
const out = path.resolve(outArg ?? '.');
fs.mkdirSync(out, { recursive: true });
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1280, height: 720 }, deviceScaleFactor: mode === 'video' ? 1.5 : 1 });
page.on('console', (m) => m.type() === 'error' && console.error('page:', m.text()));
page.on('pageerror', (e) => console.error('page error:', e.message));
await page.goto(`http://localhost:${port}/stage.html?render`);
await page.waitForFunction(() => window.ready);
const stage = page.locator('#stage');

if (mode === 'stills') {
  for (const t of (arg ?? '1,5,9').split(',').map(Number)) {
    await page.evaluate((t) => window.frame(t), t);
    await stage.screenshot({ path: path.join(out, `still-${t}.png`) });
  }
} else {
  const fps = Number(arg ?? 60), duration = 31;
  const ff = spawn('ffmpeg', ['-y', '-loglevel', 'error', '-f', 'image2pipe', '-framerate', String(fps), '-i', '-',
    '-vf', 'scale=1920:1080:flags=lanczos', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '18', '-preset', 'slow',
    '-movflags', '+faststart', path.join(out, 'ant-motion.mp4')], { stdio: ['pipe', 'inherit', 'inherit'] });
  for (let i = 0; i < fps * duration; i++) {
    await page.evaluate((t) => window.frame(t), i / fps);
    const png = await stage.screenshot({ type: 'png' });
    if (!ff.stdin.write(png)) await new Promise((r) => ff.stdin.once('drain', r));
    if (i % 300 === 0) console.log(`frame ${i}/${fps * duration}`);
  }
  ff.stdin.end();
  await new Promise((r) => ff.on('close', r));
}
await browser.close();
server.close();
