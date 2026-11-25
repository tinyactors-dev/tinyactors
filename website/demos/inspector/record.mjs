// Records the Inspector demo: node record.mjs <base URL> <out dir> <snapshot file>.
// The page at <base>/demo/inspector/intro.html is the first scene; then the
// Inspector opens the snapshot. It writes the video (WebM) and steps.json,
// each caption with its time from the start of the video, into <out dir>.
// Playwright and its Chromium are the environment's (require from anywhere).
import { createRequire } from 'node:module';
import { writeFileSync } from 'node:fs';
const { chromium } = createRequire(import.meta.url)('playwright');
const [BASE, OUT, SNAPSHOT] = process.argv.slice(2);
const steps = [];
let started = 0;
  const browser = await chromium.launch();
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 }, recordVideo: { dir: OUT, size: { width: 1440, height: 900 } }, colorScheme: 'light' });
  await context.addInitScript(() => {
    addEventListener('DOMContentLoaded', () => {
      const dot = document.createElement('div');
      dot.id = '__cursor';
      Object.assign(dot.style, { position: 'fixed', zIndex: 2147483647, width: '18px', height: '18px', borderRadius: '50%', background: 'rgba(31,94,74,.35)', border: '2px solid #1F5E4A', pointerEvents: 'none', transform: 'translate(-50%,-50%)', left: '-50px', top: '-50px', transition: 'left .08s linear, top .08s linear' });
      document.body.append(dot);
      addEventListener('mousemove', (e) => { dot.style.left = e.clientX + 'px'; dot.style.top = e.clientY + 'px'; }, true);
      const cap = document.createElement('div');
      cap.id = '__caption';
      Object.assign(cap.style, { position: 'fixed', zIndex: 2147483646, left: '50%', bottom: '28px', transform: 'translateX(-50%)', maxWidth: '980px', padding: '14px 22px', background: 'rgba(42,38,34,.92)', color: '#FAF6EE', font: '500 18px/1.45 "IBM Plex Sans", system-ui, sans-serif', borderRadius: '2px', boxShadow: '0 8px 24px rgba(0,0,0,.25)', opacity: '0', transition: 'opacity .3s', pointerEvents: 'none', textAlign: 'center' });
      document.body.append(cap);
    });
  });
  const page = await context.newPage();
  started = Date.now();
  const wait = (ms) => page.waitForTimeout(ms);
  const caption = async (text, ms = 3500) => {
    if (text) steps.push({ at: Math.max(0, Math.round((Date.now() - started) / 1000)), text });
    await page.evaluate((t) => { const c = document.querySelector('#__caption'); if (!c) return; c.textContent = t; c.style.opacity = t ? '1' : '0'; }, text);
    if (ms) await wait(ms);
  };
  const glide = async (locator, opts = {}) => {
    const box = await locator.boundingBox();
    if (!box) return;
    await page.mouse.move(box.x + (opts.x ?? box.width / 2), box.y + (opts.y ?? box.height / 2), { steps: 18 });
    await wait(250);
  };
  const click = async (locator) => { await glide(locator); await locator.click(); await wait(500); };

  // 1. The system
  await page.goto(BASE + '/demo/inspector/intro.html'); await page.waitForFunction(() => window.ready); await wait(800);
  await caption('A support desk in tinyactors: 120 tickets, each an actor running a statechart, each open ticket watched by an invoked SLA timer.', 5500);
  await caption('The workbench, tinyactors run, drives it on a simulated clock: tickets open, get triaged, assigned and answered; urgent ones breach their SLA.', 0);
  await page.evaluate(() => window.play()); await wait(1500);
  await caption('Last, run.snapshot writes the whole host to desk.snapshot: every segment’s memory, the chart images, the trace history, and a description of its own layout.', 5500);

  // 2. The Inspector
  await page.goto(BASE + '/inspector/'); await wait(1500);
  await caption('The Inspector reads a snapshot in this browser tab, using only the layout description the file carries. Nothing is uploaded.', 4500);
  const open = page.getByRole('button', { name: 'Open a snapshot' });
  await glide(open);
  await page.setInputFiles('input[type=file]', SNAPSHOT); await wait(1500);
  await caption('desk.snapshot: 208 actors, the 120 tickets and the SLA timers they invoked, in one segment of 1.8 MB. Library and layout id come from the file.', 5000);

  // 3. Memory
  const strip = page.locator('.insp__map');
  await caption('The segment’s region, byte for byte: the system header, then each pool, its generations, the indexes, the slab pages and the trace journal.', 0);
  const sb = await strip.boundingBox();
  for (let i = 0; i <= 10; i++) { await page.mouse.move(sb.x + sb.width * i / 10, sb.y + sb.height / 2, { steps: 6 }); await wait(350); }
  await wait(1200);
  await caption('One cell per actor slot, coloured by state: idle, runnable with an event waiting, hibernated to the store.', 4000);

  // 4. An actor
  const filter = page.getByPlaceholder(/Filter/);
  await click(filter);
  await caption('Find a ticket by name. ticket-4161 has a customer reply waiting in its mailbox.', 0);
  await filter.pressSequentially('ticket-4161', { delay: 90 }); await wait(800);
  await click(page.locator('text=ticket-4161').first());
  await caption('Its 48-byte record, field by field: links are slots of other pools, strings are read from the slab and the image.', 5000);
  const mail = page.locator('.insp__link', { hasText: /events\[/ }).first();
  if (await mail.count()) { await click(mail); await caption('The queued event: customer.replied, its name read from the chart image’s event table.', 4500); await page.keyboard.press('Escape'); await wait(600); }

  // 5. Chart
  await click(page.locator('button[data-center="chart"]').first());
  await caption('Its chart, decoded from the image in the snapshot, with the active states lit: open › waiting_on_customer.', 4500);
  await page.mouse.wheel(0, 400); await wait(800);
  await caption('The transitions out of the active states, and how many tickets sit in each state across the host.', 4500);
  const bar = page.locator('.insp__census-row', { hasText: 'working' }).first();
  if (await bar.count()) { await filter.fill(''); await wait(300); await click(bar); await caption('Click a state to list its tickets: the 35 that an agent is working on.', 4000); }
  await page.mouse.wheel(0, -800);

  // 6. Traces
  await click(page.locator('button[data-center="traces"]').first());
  await caption('The trace history the snapshot kept: 3,709 records, spans as OpenTelemetry names them, from the spawn to the capture.', 4500);
  const only = page.getByLabel('This actor only');
  if (await only.count()) { await click(only); await caption('Only the selected ticket: its spawn, every macrostep and microstep, each event that moved it.', 4500); }
  const row = page.locator('ta-trace-view button, ta-trace-view [role=button]').nth(2);
  if (await row.count()) { await click(row); await wait(800); }
  const name = page.locator('ta-trace-view .trace-name, ta-trace-view .trace-row__name').nth(3);
  if (await name.count()) { await click(name); await caption('A span’s details: its state, the event that caused it, its trace and parent ids.', 5000); }

  // 7. Atlas
  await caption('', 0);
  await page.goto(BASE + '/architecture/#type-ta_actor'); await wait(1200);
  await page.locator('#atlas').scrollIntoViewIfNeeded(); await wait(800);
  await caption('And Atlas, on the Architecture page: the same ta_actor record, drawn from the build’s debug information, byte for byte.', 5000);
  await caption('', 600);
  await context.close(); await browser.close();
writeFileSync(OUT + '/steps.json', JSON.stringify(steps, null, 1) + '\n');
