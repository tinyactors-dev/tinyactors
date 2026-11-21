// Presence in the page: the ant menu, and this tab's ant (website/docs/presence.md).
// The control flow is a statechart, link.scxml, run on @tinyactors/wasm:
// this module only hands it events (the tab's visibility, the switch, a tap,
// what the socket says) and gives it its I/O processors (the Worker's
// ticket, the WebSocket, the canvas, the greeting line, the remembered
// switch). Nothing loads until the page has: the ants are the background.
const root = document.querySelector('.presence');
const PREF = 'ta-presence', TAB = 'ta-presence-tab';
// Storage can throw (blocked site data): then nothing is remembered.
const storage = (name) => { try { return globalThis[name] ?? null; } catch { return null; } };
const local = storage('localStorage'), session = storage('sessionStorage');
const read = (area, key) => { try { return area?.getItem(key) ?? null; } catch { return null; } };
const write = (area, key, value) => { try { area?.setItem(key, value); } catch { /* private mode: not remembered */ } };

if (root) setup(root);

function setup(root) {
  const toggle = root.querySelector('[role="switch"]');
  const greetButton = root.querySelector('.presence__greet');
  const status = root.querySelector('.presence__status');
  const toast = root.querySelector('.presence__toast');
  const count = root.querySelector('.presence__count');
  const number = root.querySelector('.presence__number');
  let enabled = read(local, PREF) !== 'off';
  toggle.setAttribute('aria-checked', String(enabled));
  root.dataset.state = enabled ? 'connecting' : 'off';

  // Until the chart runs, the switch is remembered only.
  let link = null;
  toggle.addEventListener('click', () => {
    enabled = !enabled;
    toggle.setAttribute('aria-checked', String(enabled));
    if (link) link.send(enabled ? 'enable' : 'disable');
    else write(local, PREF, enabled ? 'on' : 'off');
  });

  const begin = () => start().then((actor) => { link = actor; }, (error) => console.warn('presence:', error));
  if (document.readyState === 'complete') idle(begin); else addEventListener('load', () => idle(begin), { once: true });

  async function start() {
    const [tinyactors, chart, svgText] = await Promise.all([
      import('@tinyactors/wasm'),
      fetch(new URL('./link.scxml', import.meta.url)).then((r) => r.text()),
      fetch('/assets/brand/ant.svg').then((r) => r.text()),
    ]);
    const { createLayer } = await import('./layer.mjs');
    const layer = createLayer({ svgText, onTap: (h) => actor.send('tap', { h }) });
    const regions = new Intl.DisplayNames([navigator.language, 'en'], { type: 'region' });
    let socket = null, tab = read(session, TAB) ?? renew(), toastTimer = 0;

    function renew() {
      const id = Array.from(crypto.getRandomValues(new Uint8Array(16)), (b) => (b % 36).toString(36)).join('');
      write(session, TAB, id);
      return id;
    }
    const sendOnSocket = (message) => { if (socket?.readyState === 1) socket.send(JSON.stringify(message)); };

    const processors = {
      'urn:ticket': {
        async receive(send) { // answered after the fetch: the chart waits in ticketing
          try {
            const response = await fetch('/presence/ticket', { cache: 'no-store' });
            if (!response.ok) throw new Error(String(response.status));
            send.answer('ticket', { ticket: (await response.json()).ticket });
          } catch { send.fail(); }
        },
      },
      'urn:socket': {
        receive(send) {
          switch (send.event) {
            case 'open': {
              socket?.close();
              const ws = new WebSocket(root.dataset.server);
              socket = ws;
              const ticket = send.data.ticket;
              ws.onopen = () => ws.send(JSON.stringify({ t: 'hello', ticket, tab }));
              ws.onmessage = ({ data }) => {
                const message = JSON.parse(data);
                if (message.t === 'welcome') { layer.welcome(message); actor.send('welcome'); }
                else if (message.t === 'batch') {
                  layer.batch(message.m);
                  for (const m of message.m) if (m.t === 'greeted') actor.send('greeted', { country: m.country });
                } else if (message.t === 'field') layer.field(message);
              };
              ws.onclose = ({ code }) => { if (socket === ws) { socket = null; actor.send('closed', { code }); } };
              return;
            }
            case 'renew': tab = renew(); return;
            case 'greet': sendOnSocket({ t: 'greet', h: send.data.h }); return;
            case 'hide': sendOnSocket({ t: 'hide' }); return;
            case 'show': sendOnSocket({ t: 'show' }); return;
            case 'leave': { const ws = socket; socket = null; if (ws?.readyState === 1) { ws.send(JSON.stringify({ t: 'leave' })); } ws?.close(); return; }
          }
        },
      },
      'urn:layer': {
        receive(send) {
          root.dataset.state = send.event; // joined, offline, off: the menu shows it
          if (send.event === 'joined') layer.show(); else layer.hide();
        },
      },
      'urn:status': {
        receive(send) {
          const country = send.data.country;
          let name = null;
          try { name = country ? regions.of(country) : null; } catch { /* not a region code */ }
          const text = name ? root.dataset.greeted.replace('{country}', name) : root.dataset.greetedAnonymous;
          status.textContent = text;
          toast.textContent = text;
          toast.hidden = false;
          clearTimeout(toastTimer);
          toastTimer = setTimeout(() => { toast.hidden = true; }, 6000);
        },
      },
      'urn:prefs': { receive(send) { write(local, PREF, send.event === 'enabled' ? 'on' : 'off'); } },
    };

    const compiled = tinyactors.compileCharts([{ name: 'link.scxml', text: chart }]);
    const host = new tinyactors.Host(compiled, { system: 'page', ioProcessors: processors });
    compiled.close();
    const actor = host.spawn('link', { params: { enabled, hidden: document.hidden } });
    document.addEventListener('visibilitychange', () => actor.send(document.hidden ? 'hide' : 'show'));
    // Going to another page of the site drops the socket without a leave: the
    // server keeps the ant 20 s, and the next page's hello finds it again.
    greetButton.addEventListener('click', () => { const h = layer.someone(); if (h) actor.send('tap', { h }); });
    // The count: the ant mark and a number; its sentence is what a screen reader says.
    const counted = () => {
      const joined = root.dataset.state === 'joined';
      count.hidden = !joined;
      if (!joined) return;
      number.textContent = String(layer.count());
      count.setAttribute('aria-label', root.dataset.count.replace('{ants}', number.textContent));
    };
    setInterval(counted, 2000);
    root.querySelector('.presence__button').addEventListener('click', counted);
    return actor;
  }
}

function idle(callback) {
  if ('requestIdleCallback' in globalThis) requestIdleCallback(callback, { timeout: 3000 });
  else setTimeout(callback, 500);
}
