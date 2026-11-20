// node run-chromium.cjs 100000 [worker]: serves this directory, opens
// page.html in headless Chromium, prints the bench result.
const http = require('http'), fs = require('fs'), path = require('path');
const { chromium } = require('playwright');
const types = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript', '.wasm': 'application/wasm' };
const server = http.createServer((req, res) => {
  const file = path.join(__dirname, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'content-type': types[path.extname(file)] ?? 'application/octet-stream' }); res.end(data);
  });
}).listen(0, async () => {
  const [n, where, dm] = process.argv.slice(2);
  const browser = await chromium.launch({ args: ['--enable-precise-memory-info'] });
  const page = await browser.newPage();
  page.on('console', (m) => console.error('console:', m.text()));
  page.on('pageerror', (e) => console.error('pageerror:', e.message));
  await page.goto(`http://localhost:${server.address().port}/page.html?n=${n}&seconds=10${where === 'worker' ? '&worker=1' : ''}${dm ? '&datamodel=' + dm : ''}`);
  await page.waitForFunction(() => window.result, null, { timeout: 0, polling: 500 });
  console.log(JSON.stringify({ browser: 'chromium ' + browser.version(), ...(await page.evaluate(() => window.result)) }));
  await browser.close(); server.close();
});
