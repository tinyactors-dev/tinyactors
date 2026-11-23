// The live chat's page: the tutorial's charts (charts/step-5 and
// charts/step-8) on @tinyactors/wasm in this tab, behind a chat window laid out as team
// chat clients are: a top bar with search, a rail of tabs with your avatar
// at its foot (its menu signs you in or out and switches the person you
// are), the rooms and people in a sidebar, and the room with its messages
// and composer. You are one of the people at a time; what the window shows
// is that person's screen, built only from what the charts sent to them
// (chat.mjs). A debugger pane, closed at first, holds the demo's own
// controls: the simulated clock; experiments (the charts' version, switched
// by a live migration, scripted scenarios, lost connections and faulty
// screens); any actor's chart lit in the states its host reports
// (components/live-chart.mjs); and the log of what happened.
import { createChat, LATEST, PEOPLE, SCENARIOS, VERSIONS, WORKSPACE } from './chat.mjs';
import { liveChart, statePath } from '/assets/components/live-chart.mjs';

const CHARTS = ['workspace.scxml', 'room.scxml', 'user.scxml'];
const SPEEDS = [1, 10, 60];
const TICK = 250; // wall milliseconds between clock steps while it runs
const AWAY = 300_000; // the user chart's timer
const clock = (seconds) => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, '0')}`;
const STATUS = { active: 'active', away: 'away', offline: 'offline', reconnecting: 'reconnecting' };
const LABEL = { active: 'Active', away: 'Away', offline: 'Offline', reconnecting: 'Connecting…' };
const LOSSES = [[5, '5 s'], [30, '30 s'], [120, '2 min']]; // how long a lost connection lasts
const DROPS = [0, 0.25, 0.5, 1]; // the share of deliveries a faulty screen loses
const DELAYS = [0, 2, 10]; // seconds a faulty screen gets each delivery late
const KINDS = { input: 'Input', screen: 'Screen', lost: 'Lost', late: 'Late', fault: 'Fault', scenario: 'Scenario', version: 'Version', data: 'Data' };
const TYPED = 2; // simulated seconds between two typing notices the page sends
const GROUP = 300; // simulated seconds within which one author's messages share a header
const COLOURS = ['#c5405b', '#d2722a', '#2e8b57', '#1f7f8f', '#3567a8', '#7a4fa3', '#a8457e', '#6b7d2a', '#9a6a12']; // avatars', one picked by the name
const SVG = 'http://www.w3.org/2000/svg';

// The icons, drawn for this page on a 20-unit grid: [stroked paths, filled circles].
const ICONS = {
  home: [['M3.5 9 10 3.5 16.5 9v7.5a1 1 0 0 1-1 1H12v-5H8v5H4.5a1 1 0 0 1-1-1z']],
  dms: [['M2.5 4.5h10v7h-5l-3 2.5v-2.5h-2z', 'M14.5 7.5h3v7h-1.5V17l-3-2.5H9']],
  activity: [['M10 3a4.5 4.5 0 0 0-4.5 4.5v3.2L4 13.5h12l-1.5-2.8V7.5A4.5 4.5 0 0 0 10 3z', 'M8 16a2 2 0 0 0 4 0']],
  more: [[], [[4.5, 10], [10, 10], [15.5, 10]]],
  search: [['M8.5 3.5a5 5 0 1 0 0 10 5 5 0 0 0 0-10z', 'm12.2 12.2 4.3 4.3']],
  bug: [['M6.5 10a3.5 3.5 0 0 1 7 0v2.5a3.5 3.5 0 0 1-7 0z', 'M8 6.8a2 2 0 0 1 4 0', 'M8.6 5 7.6 3.2M11.4 5l1-1.8', 'M6.5 11H3.5M13.5 11h3M6.8 14.2 4.2 15.6M13.2 14.2l2.6 1.4M6.9 8.4 4.5 7M13.1 8.4 15.5 7']],
  chevron: [['m6 8 4 4 4-4']],
  close: [['m5 5 10 10M15 5 5 15']],
  back: [['M12.5 4 6.5 10l6 6']],
};

/** One of ICONS, in the text's colour. */
function icon(name) {
  const [paths, dots = []] = ICONS[name];
  const svg = document.createElementNS(SVG, 'svg');
  svg.setAttribute('viewBox', '0 0 20 20');
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('class', `chatlive__icon chatlive__icon--${name}`);
  for (const d of paths) {
    const path = document.createElementNS(SVG, 'path');
    path.setAttribute('d', d);
    svg.append(path);
  }
  for (const [cx, cy] of dots) {
    const dot = document.createElementNS(SVG, 'circle');
    Object.entries({ cx, cy, r: 1.6 }).forEach(([key, value]) => dot.setAttribute(key, value));
    svg.append(dot);
  }
  return svg;
}

/** A rounded square with the name's initial, on a colour the name picks. */
function avatar(name, size) {
  let hash = 0;
  for (const char of name) hash = (hash * 31 + char.codePointAt(0)) >>> 0;
  return el('span', { class: `chatlive__avatar chatlive__avatar--${size}`, style: `background: ${COLOURS[hash % COLOURS.length]}`, 'aria-hidden': 'true' },
    name[0].toUpperCase());
}

/** An avatar with a presence dot; `presence(face, state)` sets the dot. */
function face(name, size) {
  return el('span', { class: 'chatlive__face' }, avatar(name, size), el('span', { class: 'chatlive__dot', 'aria-hidden': 'true' }));
}
function presence(node, state) {
  node.className = `chatlive__face chatlive__face--${state}`;
}

/** The send button's arrow. */
function arrow() {
  const svg = document.createElementNS(SVG, 'svg');
  svg.setAttribute('viewBox', '0 0 20 20');
  svg.setAttribute('aria-hidden', 'true');
  const path = document.createElementNS(SVG, 'path');
  path.setAttribute('d', 'M2.5 2.8 18 10 2.5 17.2l2.2-6.4L12 10l-7.3-.8z');
  svg.append(path);
  return svg;
}

/** A sidebar section that folds, as a client's do. */
function section(title, ...children) {
  return el('details', { class: 'chatlive__section', open: '' }, el('summary', { class: 'chatlive__heading' }, title), ...children);
}

/**
 * Keeps `list`'s children one per key, made by `make(key)` and brought up
 * to date by `update(node, key)`, without replacing a node whose key stays:
 * a button that has focus keeps it while the clock redraws the page.
 */
function keyed(list, keys, make, update) {
  if ([...list.children].map((node) => node.dataset.key).join('\n') !== keys.join('\n')) {
    list.replaceChildren(...keys.map((key) => {
      const node = make(key);
      node.dataset.key = key;
      return node;
    }));
  }
  for (const node of list.children) update(node, node.dataset.key);
}

/** `text` with each case-insensitive match of `query` marked. */
function marked(text, query) {
  if (!query) return [text];
  const parts = [];
  const lower = text.toLowerCase();
  let at = 0;
  for (let found = lower.indexOf(query); found !== -1; found = lower.indexOf(query, at)) {
    parts.push(text.slice(at, found), el('mark', {}, text.slice(found, found + query.length)));
    at = found + query.length;
  }
  parts.push(text.slice(at));
  return parts;
}

export async function mount(root) {
  root.classList.add('chatlive');
  const status = el('p', { class: 'chatlive__status', 'aria-live': 'polite' }, 'Loading tinyactors…');
  root.append(status);
  // both versions of the charts: file name to text, by the tutorial's step
  const texts = Object.fromEntries(await Promise.all(Object.keys(VERSIONS).map(async (step) => {
    const base = new URL(`./charts/step-${step}/`, import.meta.url);
    return [step, Object.fromEntries(await Promise.all(CHARTS.map(async (name) => [name, await (await fetch(new URL(name, base))).text()])))];
  })));
  const tinyactors = await import('@tinyactors/wasm');

  let you = 'forager', room = 'general', speed = 1, playing = true, rendering = false, again = false;
  let ready = false; // the page draws once its parts are made
  let inspected = null; // the actor whose chart is shown
  let selfState = null; // your user actor's state value: { online: ... }, 'reconnecting', or null (offline)
  let shownStored = null; // whether the inspected actor was in the store when last drawn
  let query = ''; // the search, lowercased
  let part = 'experiments'; // the debugger's tab
  let migrating = false; // a version switch is under way
  const chat = await createChat(tinyactors, { versions: texts, version: LATEST, onChange: () => render() });
  // The people already talking: scout and forager in general, scout's find.
  await chat.connect('scout');
  await chat.connect('forager');
  await chat.join('scout', 'general');
  await chat.join('forager', 'general');
  await chat.post('scout', 'general', 'Found sugar by the old oak. Who can carry?');

  const app = el('div', { class: 'chatlive__app', 'data-view': 'room' });

  // ---- the top bar: search, and the debugger's switch
  const search = el('input', { type: 'search', class: 'chatlive__search-field', placeholder: `Search ${WORKSPACE}`, 'aria-label': 'Search this channel', maxlength: '80' });
  search.addEventListener('input', () => { query = search.value.trim().toLowerCase(); render(); });
  search.addEventListener('keydown', (event) => { if (event.key === 'Escape' && search.value) { search.value = ''; query = ''; render(); } });
  const debugToggle = el('button', { type: 'button', class: 'chatlive__top-button', 'aria-label': 'Debugger', 'aria-pressed': 'false', title: 'Debugger and experiments' },
    icon('bug'), el('span', { class: 'chatlive__top-label' }, 'Debugger'));
  debugToggle.addEventListener('click', () => openDebugger(debugToggle.getAttribute('aria-pressed') !== 'true'));
  const top = el('div', { class: 'chatlive__top' },
    el('label', { class: 'chatlive__search' }, icon('search'), search),
    debugToggle);

  // ---- the rail: the workspace, the tabs, you
  const tab = (name, label, current) => {
    const button = el('button', { type: 'button', class: 'chatlive__tab' }, el('span', { class: 'chatlive__tab-icon' }, icon(name)), el('span', { class: 'chatlive__tab-label' }, label));
    if (current) button.setAttribute('aria-current', 'page');
    else button.setAttribute('aria-disabled', 'true');
    return button;
  };
  const home = tab('home', 'Home', true);
  home.addEventListener('click', () => { app.dataset.view = 'list'; });
  const meFace = face(you, 'rail');
  const me = el('button', { type: 'button', class: 'chatlive__me', 'aria-haspopup': 'menu', 'aria-expanded': 'false', 'aria-controls': 'chatlive-menu' }, meFace);
  const rail = el('nav', { class: 'chatlive__rail', 'aria-label': 'Tabs' },
    el('span', { class: 'chatlive__tile', title: WORKSPACE, 'aria-hidden': 'true' }, WORKSPACE.replace(/^The /, '')[0]),
    home, tab('dms', 'DMs'), tab('activity', 'Activity'), tab('more', 'More'),
    me);

  // ---- your menu: who you are, signing in or out, switching person
  const menuFace = face(you, 'large');
  const menuName = el('strong', { class: 'chatlive__menu-name' });
  const menuStatus = el('span', { class: 'chatlive__menu-status' });
  const sign = el('button', { type: 'button', role: 'menuitem', tabindex: '-1', class: 'chatlive__item' });
  sign.addEventListener('click', () => { closeMenu(true); return selfState ? chat.disconnect(you) : chat.connect(you); });
  const switchList = el('div', { role: 'group', 'aria-labelledby': 'chatlive-switch', class: 'chatlive__switch' },
    ...PEOPLE.map((name) => {
      const item = el('button', { type: 'button', role: 'menuitemradio', tabindex: '-1', class: 'chatlive__item chatlive__item--person', 'data-key': name },
        face(name, 'small'), el('span', { class: 'chatlive__name' }, name), el('span', { class: 'chatlive__item-note' }));
      item.addEventListener('click', () => {
        closeMenu(true);
        if (name === you) return;
        you = name;
        inspect(`user.${you}`);
      });
      return item;
    }));
  const menuList = el('div', { role: 'menu', id: 'chatlive-menu', 'aria-label': 'You', class: 'chatlive__menu-list' },
    sign, el('div', { role: 'separator', class: 'chatlive__rule' }),
    el('div', { class: 'chatlive__menu-label', id: 'chatlive-switch' }, 'Switch person'), switchList);
  const menu = el('div', { class: 'chatlive__menu', hidden: '' },
    el('div', { class: 'chatlive__menu-head' }, menuFace, el('span', { class: 'chatlive__menu-who' }, menuName, menuStatus)),
    el('div', { class: 'chatlive__rule', 'aria-hidden': 'true' }), menuList);
  const items = () => [...menu.querySelectorAll('[role^="menuitem"]')];
  function openMenu() {
    menu.hidden = false;
    me.setAttribute('aria-expanded', 'true');
    render();
    items()[0].focus();
  }
  function closeMenu(refocus) {
    if (menu.hidden) return;
    menu.hidden = true;
    me.setAttribute('aria-expanded', 'false');
    if (refocus) me.focus();
  }
  me.addEventListener('click', () => (menu.hidden ? openMenu() : closeMenu(true)));
  menu.addEventListener('keydown', (event) => {
    const all = items();
    const at = all.indexOf(document.activeElement);
    const move = { ArrowDown: at + 1, ArrowUp: at - 1, Home: 0, End: all.length - 1 }[event.key];
    if (move !== undefined) {
      event.preventDefault();
      all[(move + all.length) % all.length].focus();
    } else if (event.key === 'Escape') {
      event.preventDefault();
      closeMenu(true);
    } else if (event.key === 'Tab') closeMenu(false);
  });
  document.addEventListener('pointerdown', (event) => {
    if (!menu.hidden && !menu.contains(event.target) && !me.contains(event.target)) closeMenu(false);
  });

  // ---- the sidebar: the rooms, the people
  const roomList = el('ul', { class: 'chatlive__rooms', 'aria-label': 'Channels' });
  const newRoom = el('input', { type: 'text', class: 'chatlive__new', placeholder: 'Add channels', 'aria-label': 'New channel name', maxlength: '40' });
  const create = el('button', { type: 'submit', class: 'chatlive__add' }, 'Add');
  const createForm = el('form', { class: 'chatlive__create' }, el('span', { class: 'chatlive__plus', 'aria-hidden': 'true' }, '+'), newRoom, create);
  const createNote = el('p', { class: 'chatlive__note', 'aria-live': 'polite' });
  createForm.addEventListener('submit', async (event) => {
    event.preventDefault();
    const name = newRoom.value.trim();
    if (!name) return;
    const [answer, data] = await chat.createRoom(name);
    createNote.textContent = answer === 'room.created' ? `The workspace made #${name}.` : `The workspace refused #${name}: ${data.reason}.`;
    if (answer === 'room.created') { newRoom.value = ''; room = name; }
    render();
  });
  const people = el('ul', { class: 'chatlive__people', 'aria-label': 'People' });
  const sidebar = el('nav', { class: 'chatlive__sidebar', 'aria-label': WORKSPACE },
    el('h3', { class: 'chatlive__workspace' }, WORKSPACE, icon('chevron')),
    section('Channels', roomList, createForm, createNote),
    section('People', people));

  // ---- the room you look at
  const roomTitle = el('h3', { class: 'chatlive__title' });
  const back = el('button', { type: 'button', class: 'chatlive__back', 'aria-label': 'Channels' }, icon('back'));
  back.addEventListener('click', () => { app.dataset.view = 'list'; });
  const banner = el('p', { class: 'chatlive__banner', role: 'status', hidden: '' },
    el('strong', {}, 'Connecting…'), ' For 30 seconds, new messages wait for you.');
  const messages = el('ol', { class: 'chatlive__messages', 'aria-live': 'polite', 'aria-label': 'Messages' });
  const text = el('input', { type: 'text', class: 'chatlive__text', 'aria-label': 'Message', maxlength: '200' });
  const send = el('button', { type: 'submit', class: 'chatlive__send', 'aria-label': 'Send' }, arrow());
  const join = el('button', { type: 'button', class: 'chatlive__join' });
  join.addEventListener('click', () => chat.join(you, room));
  const signIn = el('button', { type: 'button', class: 'chatlive__join' }, 'Sign in');
  signIn.addEventListener('click', () => chat.connect(you));
  const signedOut = el('div', { class: 'chatlive__signed-out' }, el('p', {}, 'You’re signed out. Sign in to send messages.'), signIn);
  const composer = el('form', { class: 'chatlive__composer' }, text, send);
  const typingLine = el('p', { class: 'chatlive__typing', 'aria-live': 'polite' });
  let typed = -Infinity; // the simulated time of the last typing notice sent
  text.addEventListener('input', () => {
    send.classList.toggle('chatlive__send--ready', text.value.trim() !== '');
    if (!selfState?.online || chat.now() - typed < TYPED) return; // the chart throttles too: one notice to the room per 3 s
    typed = chat.now();
    chat.typing(you, room);
  });
  composer.addEventListener('submit', (event) => {
    event.preventDefault();
    const words = text.value.trim();
    if (!words || text.disabled) return;
    text.value = '';
    send.classList.remove('chatlive__send--ready');
    typed = -Infinity;
    chat.post(you, room, words);
  });
  const main = el('section', { class: 'chatlive__room', 'aria-label': 'Channel' }, el('header', { class: 'chatlive__bar-title' }, back, roomTitle), banner, messages,
    el('div', { class: 'chatlive__foot' }, composer, join, signedOut, typingLine));

  // ---- the debugger: the clock, then its parts behind tabs
  const play = el('button', { type: 'button', class: 'chatlive__control' });
  const speeds = el('div', { class: 'chatlive__speeds', role: 'radiogroup', 'aria-label': 'Clock speed' });
  for (const value of SPEEDS) {
    const button = el('button', { type: 'button', role: 'radio', class: 'chatlive__speed', 'aria-checked': String(value === speed), 'data-speed': String(value) }, `${value}×`);
    button.addEventListener('click', () => setClock(playing, value));
    speeds.append(button);
  }
  const skip = el('button', { type: 'button', class: 'chatlive__control' }, 'Skip 5 minutes');
  const time = el('output', { class: 'chatlive__time' });
  function setClock(run, value) {
    playing = run;
    speed = value;
    play.textContent = playing ? 'Pause' : 'Run';
    for (const button of speeds.children) button.setAttribute('aria-checked', String(Number(button.dataset.speed) === speed));
  }
  play.addEventListener('click', () => setClock(!playing, speed));
  setClock(playing, speed);
  skip.addEventListener('click', () => chat.advance(AWAY));
  let ticking = false;
  setInterval(async () => {
    if (!playing || ticking || !root.isConnected) return;
    ticking = true;
    try { await chat.advance(TICK * speed); } finally { ticking = false; }
  }, TICK);

  // the version of the charts, switched by a live migration
  const versionPick = el('div', { class: 'chatlive__speeds chatlive__versions', role: 'radiogroup', 'aria-label': 'Charts' },
    ...Object.keys(VERSIONS).map((step) => {
      const button = el('button', { type: 'button', role: 'radio', class: 'chatlive__speed', 'data-step': step }, `Step ${step}`);
      button.addEventListener('click', () => useVersion(Number(step)));
      return button;
    }));
  const versionAbout = el('p', { class: 'chatlive__hint' });
  const versionNote = el('p', { class: 'chatlive__result', 'aria-live': 'polite' });
  async function useVersion(step) {
    if (migrating || step === chat.version()) return;
    migrating = true;
    versionNote.textContent = `Migrating every actor to step ${step}…`;
    render();
    try {
      const result = await chat.useVersion(step);
      const pending = Array.isArray(result?.pending) ? result.pending.length : (result?.pending ?? 0);
      versionNote.textContent = `Migrated ${result?.migrated ?? 0} actors live, ${result?.failed?.length ?? 0} failed${pending ? `, ${pending} pending` : ''}`;
    } catch (error) {
      versionNote.textContent = `The migration stopped: ${error.message}`;
    } finally {
      migrating = false;
    }
    showChart();
    render();
  }

  // the scripted scenarios
  const scenarioList = el('ul', { class: 'chatlive__scenarios' },
    ...Object.entries(SCENARIOS).map(([id, scenario]) => {
      const run = el('button', { type: 'button', class: 'chatlive__control' }, 'Run');
      run.addEventListener('click', () => {
        setClock(true, 1);
        chat.scenario(id);
      });
      return el('li', { class: 'chatlive__card', 'data-key': id },
        el('div', { class: 'chatlive__card-text' }, el('strong', {}, scenario.title), el('span', {}, scenario.about), el('span', { class: 'chatlive__card-note' })),
        run);
    }));

  // each person: a lost connection, and the faults of their screen
  const select = (label, values, show, change) => {
    const field = el('select', { class: 'chatlive__select' }, ...values.map((value) => el('option', { value: String(value) }, show(value))));
    field.addEventListener('change', () => change(Number(field.value)));
    return el('label', { class: 'chatlive__field' }, label, field);
  };
  const personList = el('ul', { class: 'chatlive__lab-people', 'aria-label': 'People' },
    ...PEOPLE.map((name) => {
      const faults = () => chat.faults(name);
      return el('li', { class: 'chatlive__lab-person', 'data-key': name },
        el('div', { class: 'chatlive__lab-head' }, avatar(name, 'small'), el('span', { class: 'chatlive__name' }, name), el('span', { class: 'chatlive__lab-note' })),
        el('div', { class: 'chatlive__lab-row' }, el('span', { class: 'chatlive__lab-label' }, 'Lose connection'),
          el('div', { class: 'chatlive__speeds', role: 'group', 'aria-label': `Lose ${name}’s connection for` },
            ...LOSSES.map(([seconds, label]) => {
              const button = el('button', { type: 'button', class: 'chatlive__speed', 'aria-label': `Lose ${name}’s connection for ${label}` }, label);
              button.addEventListener('click', () => chat.loseConnection(name, seconds));
              return button;
            }))),
        el('div', { class: 'chatlive__lab-row' }, el('span', { class: 'chatlive__lab-label' }, 'Screen'),
          select('drops', DROPS, (value) => `${Math.round(value * 100)}%`, (drop) => chat.setFaults(name, { ...faults(), drop })),
          select('delay', DELAYS, (value) => `${value} s`, (delay) => chat.setFaults(name, { ...faults(), delay }))));
    }));

  const experiments = el('div', { class: 'chatlive__panel', role: 'tabpanel', id: 'chatlive-experiments', 'aria-labelledby': 'chatlive-tab-experiments' },
    el('h4', { class: 'chatlive__subheading' }, 'Charts'), versionPick, versionAbout,
    el('p', { class: 'chatlive__hint' }, 'A switch migrates every running actor to the other charts, without a restart.'), versionNote,
    el('h4', { class: 'chatlive__subheading' }, 'Scenarios'), scenarioList,
    el('h4', { class: 'chatlive__subheading' }, 'People'), personList);

  // the actors, and one actor's chart
  const memory = el('p', { class: 'chatlive__memory' });
  const actorList = el('ul', { class: 'chatlive__actors', 'aria-label': 'Actors' });
  const frame = el('div', { class: 'chatlive__chart' });
  const stateLine = el('p', { class: 'chatlive__state', 'aria-live': 'polite' });
  const actorsPart = el('div', { class: 'chatlive__panel', role: 'tabpanel', id: 'chatlive-actors', 'aria-labelledby': 'chatlive-tab-actors', hidden: '' },
    memory, actorList, frame, stateLine);

  // the log, newest first
  const kindPick = el('select', { class: 'chatlive__select', 'aria-label': 'Kind' },
    el('option', { value: '' }, 'Every kind'), ...Object.entries(KINDS).map(([kind, label]) => el('option', { value: kind }, label)));
  kindPick.addEventListener('change', () => render());
  const logList = el('ol', { class: 'chatlive__log', 'aria-label': 'Log' });
  let logDrawn = '';
  const logPart = el('div', { class: 'chatlive__panel', role: 'tabpanel', id: 'chatlive-log', 'aria-labelledby': 'chatlive-tab-log', hidden: '' },
    el('div', { class: 'chatlive__log-head' }, el('p', { class: 'chatlive__hint' }, 'The newest 200 entries, at simulated time.'), kindPick), logList);

  const parts = { experiments, actors: actorsPart, log: logPart };
  const tabs = el('div', { class: 'chatlive__tabs', role: 'tablist', 'aria-label': 'Debugger' },
    ...Object.entries({ experiments: 'Experiments', actors: 'Actors', log: 'Log' }).map(([key, label]) => {
      const button = el('button', { type: 'button', role: 'tab', class: 'chatlive__debug-tab', id: `chatlive-tab-${key}`, 'aria-controls': `chatlive-${key}`,
        'aria-selected': String(key === part), tabindex: key === part ? '0' : '-1', 'data-key': key }, label);
      button.addEventListener('click', () => showPart(key));
      return button;
    }));
  tabs.addEventListener('keydown', (event) => {
    const keys = Object.keys(parts);
    const step = { ArrowRight: 1, ArrowLeft: -1 }[event.key];
    if (!step) return;
    event.preventDefault();
    const next = keys[(keys.indexOf(part) + step + keys.length) % keys.length];
    showPart(next);
    tabs.querySelector(`[data-key="${next}"]`).focus();
  });
  function showPart(key) {
    part = key;
    for (const button of tabs.children) {
      button.setAttribute('aria-selected', String(button.dataset.key === key));
      button.tabIndex = button.dataset.key === key ? 0 : -1;
    }
    for (const [name, panel] of Object.entries(parts)) panel.hidden = name !== key;
    logDrawn = '';
    showChart();
    render();
  }

  const closeDebugger = el('button', { type: 'button', class: 'chatlive__close', 'aria-label': 'Close debugger' }, icon('close'));
  closeDebugger.addEventListener('click', () => { openDebugger(false); debugToggle.focus(); });
  const debug = el('aside', { class: 'chatlive__debug', 'aria-label': 'Debugger', hidden: '' },
    el('header', { class: 'chatlive__bar-title' }, el('h3', { class: 'chatlive__title' }, 'Debugger'), closeDebugger),
    el('div', { class: 'chatlive__details' },
      el('div', { class: 'chatlive__clockbar' },
        el('p', { class: 'chatlive__clock' }, 'Simulated time ', time),
        el('div', { class: 'chatlive__controls' }, play, speeds, skip)),
      tabs, experiments, actorsPart, logPart));

  // the inspected actor's chart, in the version the chat runs now
  const charts = {};
  const chartKey = () => `${chat.version()}/${inspected?.split('.')[0]}.scxml`;
  function showChart() {
    if (!inspected || debug.hidden || part !== 'actors') return; // a chart is drawn in a part that shows
    const key = chartKey();
    for (const [other, chart] of Object.entries(charts)) chart.view.hidden = other !== key;
    charts[key] ??= liveChart(texts[chat.version()][key.split('/')[1]], frame);
    charts[key].view.hidden = false;
    charts[key].restart();
    shownStored = null;
  }
  function inspect(name) {
    inspected = name;
    showChart();
    render();
  }
  function openDebugger(open) {
    debug.hidden = !open;
    debugToggle.setAttribute('aria-pressed', String(open));
    app.classList.toggle('chatlive__app--debug', open);
    if (open) { logDrawn = ''; showChart(); }
    render();
  }

  app.append(top, rail, sidebar, el('div', { class: 'chatlive__pane' }, main, debug), menu);
  root.replaceChildren(app, status);
  status.textContent = '';
  ready = true;

  // ---- what each part shows, after every change
  // Your actor's state value. While it is in the store (its state is in
  // its blob), what the workspace last told another person online about
  // you; with nobody online to tell, the last state read. A lost
  // connection on the newest charts shows no change to the others, so while the
  // agenda still holds your reconnect, the last state read stands.
  const known = new Map();
  function yours(self) {
    if (!self.stored) { known.set(you, self.value); return asSelf(self.value); }
    const last = known.get(you) ?? null;
    const mine = chat.screen(you).roster;
    const told = PEOPLE.filter((name) => name !== you && ['active', 'away'].includes(mine[name]))
      .map((name) => chat.screen(name).roster[you]).find(Boolean);
    if (told === 'offline') return null;
    if (last === 'reconnecting' && chat.agenda().some((entry) => entry.what === `${you} reconnects`)) return last;
    if (!told) return asSelf(last);
    return { online: told };
  }
  const asSelf = (value) => (value === 'offline' || value === null ? null : value);
  const statusOf = (state) => (state === 'reconnecting' ? 'reconnecting' : state?.online ?? 'offline');

  async function render() {
    if (!ready) return;
    if (rendering) { again = true; return; }
    rendering = true;
    try {
      do {
        again = false;
        await draw();
      } while (again);
    } finally { rendering = false; }
  }

  async function draw() {
    const screen = chat.screen(you);
    const showActors = !debug.hidden && part === 'actors';
    const [actors, stats, self, shown] = await Promise.all([chat.actors(), showActors ? chat.stats() : null, chat.state(`user.${you}`),
      inspected && showActors ? chat.state(inspected) : null]);
    selfState = yours(self);
    const yourStatus = statusOf(selfState);
    const reconnecting = yourStatus === 'reconnecting';
    const roster = { ...screen.roster, [you]: yourStatus };

    // you, on the rail and in your menu
    meFace.replaceChildren(...face(you, 'rail').childNodes);
    presence(meFace, yourStatus);
    me.setAttribute('aria-label', `${you}, ${LABEL[yourStatus]}`);
    menuFace.replaceChildren(...face(you, 'large').childNodes);
    presence(menuFace, yourStatus);
    menuName.textContent = you;
    menuStatus.textContent = LABEL[yourStatus];
    sign.textContent = selfState ? `Sign out of ${WORKSPACE}` : `Sign in to ${WORKSPACE}`;
    for (const item of switchList.children) {
      const name = item.dataset.key;
      const state = STATUS[roster[name]] ?? 'unknown';
      item.setAttribute('aria-checked', String(name === you));
      presence(item.firstChild, state);
      item.lastChild.textContent = name === you ? 'you' : state === 'unknown' ? '' : state;
    }

    // the sidebar
    const rooms = actors.filter((actor) => actor.chart === 'room').map((actor) => actor.name.slice(5));
    keyed(roomList, rooms, (name) => {
      const button = el('button', { type: 'button', class: 'chatlive__room-link' },
        el('span', { class: 'chatlive__hash', 'aria-hidden': 'true' }, '#'), name);
      button.addEventListener('click', () => { room = name; app.dataset.view = 'room'; render(); });
      return el('li', {}, button);
    }, (node, name) => {
      const button = node.firstChild;
      button.setAttribute('aria-current', String(name === room));
      button.classList.toggle('chatlive__room-link--joined', screen.rooms.includes(name));
    });
    people.replaceChildren(...PEOPLE.map((name) => {
      const state = STATUS[roster[name]] ?? 'unknown';
      const node = face(name, 'small');
      presence(node, state);
      return el('li', { class: `chatlive__person chatlive__person--${state}` }, node,
        el('span', { class: 'chatlive__name' }, name),
        name === you ? el('span', { class: 'chatlive__you-mark' }, 'you') : '',
        el('span', { class: 'chatlive__presence' }, state === 'unknown' ? 'not on your screen' : LABEL[state].toLowerCase()));
    }));

    // the room
    const joined = screen.rooms.includes(room);
    roomTitle.replaceChildren(el('span', { class: 'chatlive__hash', 'aria-hidden': 'true' }, '#'), room);
    const inRoom = screen.messages.filter((message) => message.room === room);
    const here = query ? inRoom.filter((message) => message.text.toLowerCase().includes(query)) : inRoom;
    const atBottom = messages.scrollHeight - messages.scrollTop - messages.clientHeight < 40;
    const empty = !joined && !inRoom.length ? `You have not joined #${room}.`
      : query && inRoom.length ? `No messages in #${room} match “${search.value.trim()}”.`
        : 'No messages on your screen yet.';
    messages.replaceChildren(...(here.length ? here.map((message, index) => {
      const before = here[index - 1];
      // a post from a room's history (step 8's join) came with no time of its own
      const stamp = el('time', { class: 'chatlive__at' }, message.at === null ? 'earlier' : clock(message.at));
      const body = el('p', { class: 'chatlive__body' }, ...marked(message.text, query));
      if (before && before.from === message.from && message.at - before.at < GROUP) {
        return el('li', { class: 'chatlive__message chatlive__message--more' }, stamp, body);
      }
      return el('li', { class: 'chatlive__message' }, avatar(message.from, 'large'),
        el('p', { class: 'chatlive__line' }, el('span', { class: 'chatlive__from' }, message.from), stamp), body);
    })
      : [el('li', { class: 'chatlive__empty' }, empty)]),
      ...screen.notices.slice(-1).map((notice) => el('li', { class: 'chatlive__notice' }, notice)));
    if (atBottom) messages.scrollTop = messages.scrollHeight;
    banner.hidden = !reconnecting;
    const connected = Boolean(selfState);
    composer.hidden = !(connected && joined);
    composer.classList.toggle('chatlive__composer--off', reconnecting);
    text.disabled = reconnecting;
    send.disabled = reconnecting;
    join.hidden = !connected || joined;
    join.disabled = reconnecting;
    signedOut.hidden = connected;
    join.textContent = `Join #${room}`;
    text.placeholder = reconnecting ? 'Connecting…' : `Message #${room}`;
    // step 5's charts send no typing notices, and no typing.stopped for those the newest sent before a switch
    const typers = connected && chat.version() === LATEST ? (screen.typing[room] ?? []).filter((name) => name !== you) : [];
    typingLine.textContent = typers.length === 0 ? ''
      : typers.length === 1 ? `${typers[0]} is typing…`
        : typers.length === 2 ? `${typers[0]} and ${typers[1]} are typing…`
          : 'Several people are typing…';

    // the debugger
    if (debug.hidden) return;
    time.textContent = clock(chat.now());
    if (part === 'experiments') {
      const version = chat.version();
      for (const button of versionPick.children) {
        button.setAttribute('aria-checked', String(Number(button.dataset.step) === version));
        button.disabled = migrating;
      }
      versionAbout.textContent = VERSIONS[version].replace(/^Step \d+: /, '').replace(/^./, (first) => first.toUpperCase());
      const agenda = chat.agenda();
      for (const card of scenarioList.children) {
        const { title } = SCENARIOS[card.dataset.key];
        const left = agenda.filter((entry) => entry.what === title).length;
        card.querySelector('.chatlive__card-note').textContent = left ? `Running: ${left} ${left === 1 ? 'step' : 'steps'} to go` : '';
      }
      for (const row of personList.children) {
        const name = row.dataset.key;
        const returns = agenda.find((entry) => entry.what === `${name} reconnects`);
        row.querySelector('.chatlive__lab-note').textContent = returns ? `Reconnects at ${clock(returns.at)}` : name === you ? 'you' : '';
        row.classList.toggle('chatlive__lab-person--lost', Boolean(returns));
        const fault = chat.faults(name);
        const [drop, delay] = row.querySelectorAll('select');
        if (document.activeElement !== drop) drop.value = String(fault.drop);
        if (document.activeElement !== delay) delay.value = String(fault.delay);
      }
    } else if (part === 'log') {
      const kind = kindPick.value;
      const entries = chat.log().filter((entry) => !kind || entry.kind === kind).reverse();
      const signature = `${kind}\n${entries.length}\n${entries[0]?.at}\n${entries[0]?.text}`;
      if (signature !== logDrawn) {
        logDrawn = signature;
        logList.replaceChildren(...(entries.length ? entries.map((entry) => el('li', { class: `chatlive__entry chatlive__entry--${entry.kind}` },
          el('time', { class: 'chatlive__at' }, clock(entry.at)), el('span', { class: 'chatlive__kind' }, KINDS[entry.kind] ?? entry.kind),
          el('span', { class: 'chatlive__entry-text' }, entry.text)))
          : [el('li', { class: 'chatlive__empty' }, 'Nothing yet. Post a message or run a scenario.')]));
      }
    } else if (part === 'actors') {
      memory.textContent = `${stats.actors} actors · ${stats.inMemory} in memory · ${stats.stored} in the store`;
      keyed(actorList, actors.map((actor) => actor.name), (name) => {
        const button = el('button', { type: 'button', class: 'chatlive__actor' },
          el('span', { class: 'chatlive__dot', 'aria-hidden': 'true' }), el('code', {}, name), el('span', { class: 'chatlive__where' }));
        button.addEventListener('click', () => inspect(name));
        return el('li', {}, button);
      }, (node, name) => {
        const actor = actors.find((each) => each.name === name);
        const button = node.firstChild;
        button.setAttribute('aria-current', String(name === inspected));
        button.classList.toggle('chatlive__actor--stored', actor.stored);
        button.lastChild.textContent = actor.stored ? 'in the store' : 'in memory';
      });
      const chart = charts[chartKey()];
      if (shown?.stored) {
        if (shownStored !== true) chart?.restart(); // nothing lit: the actor's state is in its blob
        shownStored = true;
        stateLine.textContent = `${inspected} waits in the store; its next event loads it.`;
      } else if (shown) {
        shownStored = false;
        chart?.show(shown.value);
        stateLine.replaceChildren(el('code', {}, inspected), ' is in ', el('code', {}, statePath(shown.value)), ` (step ${chat.version()})`);
      }
    }
  }

  inspect(`user.${you}`);
}

function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}
