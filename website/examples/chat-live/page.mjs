// The live chat's page: the tutorial's finished charts (charts/, step 5's)
// on @tinyactors/wasm in this tab, behind a chat window laid out as team
// chat clients are: a top bar with search, a rail of tabs with your avatar
// at its foot (its menu signs you in or out and switches the person you
// are), the rooms and people in a sidebar, and the room with its messages
// and composer. You are one of the people at a time; what the window shows
// is that person's screen, built only from what the charts sent to them
// (chat.mjs). A debugger pane, closed at first, holds the demo's own
// controls: the simulated clock, and any actor's chart lit in the states
// its host reports (components/live-chart.mjs).
import { createChat, PEOPLE, WORKSPACE } from './chat.mjs';
import { liveChart, statePath } from '/assets/components/live-chart.mjs';

const CHARTS = ['workspace.scxml', 'room.scxml', 'user.scxml'];
const SPEEDS = [1, 10, 60];
const TICK = 250; // wall milliseconds between clock steps while it runs
const AWAY = 300_000; // the user chart's timer
const clock = (seconds) => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, '0')}`;
const STATUS = { active: 'active', away: 'away', offline: 'offline' };
const LABEL = { active: 'Active', away: 'Away', offline: 'Offline' };
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
  const base = new URL('./charts/', import.meta.url);
  const texts = Object.fromEntries(await Promise.all(CHARTS.map(async (name) => [name, await (await fetch(new URL(name, base))).text()])));
  const tinyactors = await import('@tinyactors/wasm');

  let you = 'forager', room = 'general', speed = 1, playing = true, rendering = false, again = false;
  let ready = false; // the page draws once its parts are made
  let inspected = null; // the actor whose chart is shown
  let selfState = null; // your user actor's state value; null: offline
  let shownStored = null; // whether the inspected actor was in the store when last drawn
  let query = ''; // the search, lowercased
  const chat = await createChat(tinyactors, { charts: texts, onChange: () => render() });
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
  const debugToggle = el('button', { type: 'button', class: 'chatlive__top-button', 'aria-label': 'Debugger', 'aria-pressed': 'false', title: 'Debugger' }, icon('bug'));
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
  sign.addEventListener('click', () => { closeMenu(true); return online(you) ? chat.disconnect(you) : chat.connect(you); });
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
  const messages = el('ol', { class: 'chatlive__messages', 'aria-live': 'polite', 'aria-label': 'Messages' });
  const text = el('input', { type: 'text', class: 'chatlive__text', 'aria-label': 'Message', maxlength: '200' });
  const send = el('button', { type: 'submit', class: 'chatlive__send', 'aria-label': 'Send' }, arrow());
  const join = el('button', { type: 'button', class: 'chatlive__join' });
  join.addEventListener('click', () => chat.join(you, room));
  const signIn = el('button', { type: 'button', class: 'chatlive__join' }, 'Sign in');
  signIn.addEventListener('click', () => chat.connect(you));
  const signedOut = el('div', { class: 'chatlive__signed-out' }, el('p', {}, 'You’re signed out. Sign in to send messages.'), signIn);
  const composer = el('form', { class: 'chatlive__composer' }, text, send);
  const seen = new Map(); // each message's simulated time, when the page first saw it
  let typed = -Infinity; // the simulated time of the last activity sent for typing
  text.addEventListener('input', () => {
    send.classList.toggle('chatlive__send--ready', text.value.trim() !== '');
    if (!online(you) || chat.now() - typed < 5) return; // one activity per 5 simulated seconds of typing
    typed = chat.now();
    chat.activity(you);
  });
  composer.addEventListener('submit', (event) => {
    event.preventDefault();
    const words = text.value.trim();
    if (!words) return;
    text.value = '';
    send.classList.remove('chatlive__send--ready');
    chat.post(you, room, words);
  });
  const main = el('section', { class: 'chatlive__room', 'aria-label': 'Channel' }, el('header', { class: 'chatlive__bar-title' }, back, roomTitle), messages,
    el('div', { class: 'chatlive__foot' }, composer, join, signedOut));

  // ---- the debugger: the clock, and any actor's chart lit in its states
  const play = el('button', { type: 'button', class: 'chatlive__control' });
  const speeds = el('div', { class: 'chatlive__speeds', role: 'radiogroup', 'aria-label': 'Clock speed' });
  for (const value of SPEEDS) {
    const button = el('button', { type: 'button', role: 'radio', class: 'chatlive__speed', 'aria-checked': String(value === speed) }, `${value}×`);
    button.addEventListener('click', () => {
      speed = value;
      for (const other of speeds.children) other.setAttribute('aria-checked', String(other === button));
    });
    speeds.append(button);
  }
  const skip = el('button', { type: 'button', class: 'chatlive__control' }, 'Skip 5 minutes');
  const time = el('output', { class: 'chatlive__time' });
  const memory = el('p', { class: 'chatlive__memory' });
  const showPlay = () => { play.textContent = playing ? 'Pause' : 'Run'; };
  play.addEventListener('click', () => { playing = !playing; showPlay(); });
  showPlay();
  skip.addEventListener('click', () => chat.advance(AWAY));
  let ticking = false;
  setInterval(async () => {
    if (!playing || ticking || !root.isConnected) return;
    ticking = true;
    try { await chat.advance(TICK * speed); } finally { ticking = false; }
  }, TICK);

  const actorList = el('ul', { class: 'chatlive__actors', 'aria-label': 'Actors' });
  const frame = el('div', { class: 'chatlive__chart' });
  const stateLine = el('p', { class: 'chatlive__state', 'aria-live': 'polite' });
  const closeDebugger = el('button', { type: 'button', class: 'chatlive__close', 'aria-label': 'Close debugger' }, icon('close'));
  closeDebugger.addEventListener('click', () => { openDebugger(false); debugToggle.focus(); });
  const debug = el('aside', { class: 'chatlive__debug', 'aria-label': 'Debugger', hidden: '' },
    el('header', { class: 'chatlive__bar-title' }, el('h3', { class: 'chatlive__title' }, 'Debugger'), closeDebugger),
    el('div', { class: 'chatlive__details' },
      el('h4', { class: 'chatlive__subheading' }, 'Clock'),
      el('p', { class: 'chatlive__clock' }, 'Simulated time ', time),
      el('div', { class: 'chatlive__controls' }, play, speeds, skip),
      el('h4', { class: 'chatlive__subheading' }, 'Actors'), memory, actorList,
      frame, stateLine));
  const charts = {};
  function showChart() {
    if (!inspected || debug.hidden) return; // a chart is drawn in a pane that shows
    const chartName = `${inspected.split('.')[0]}.scxml`;
    for (const [key, chart] of Object.entries(charts)) chart.view.hidden = key !== chartName;
    charts[chartName] ??= liveChart(texts[chartName], frame);
    charts[chartName].view.hidden = false;
    charts[chartName].restart();
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
    if (open) showChart();
    render();
  }

  app.append(top, rail, sidebar, el('div', { class: 'chatlive__pane' }, main, debug), menu);
  root.replaceChildren(app, status);
  status.textContent = '';
  ready = true;

  // ---- what each part shows, after every change
  // Your actor's state value. While it is in the store (its state is in
  // its blob), what the workspace last told another person online about
  // you; with nobody online to tell, the last state read.
  const known = new Map();
  function yours(self) {
    if (!self.stored) { known.set(you, self.value); return self.value; }
    const mine = chat.screen(you).roster;
    const told = PEOPLE.filter((name) => name !== you && ['active', 'away'].includes(mine[name]))
      .map((name) => chat.screen(name).roster[you]).find(Boolean);
    if (!told) return known.get(you) ?? null;
    return told === 'offline' ? null : { online: told };
  }
  function online(name) { return name === you ? Boolean(selfState?.online) : chat.screen(you).roster[name] !== undefined && chat.screen(you).roster[name] !== 'offline'; }

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
    const [actors, stats, self, shown] = await Promise.all([chat.actors(), chat.stats(), chat.state(`user.${you}`),
      inspected && !debug.hidden ? chat.state(inspected) : null]);
    selfState = yours(self);
    const yourStatus = selfState?.online ?? 'offline';
    const roster = { ...screen.roster, [you]: yourStatus };

    // you, on the rail and in your menu
    meFace.replaceChildren(...face(you, 'rail').childNodes);
    presence(meFace, yourStatus);
    me.setAttribute('aria-label', `${you}, ${LABEL[yourStatus]}`);
    menuFace.replaceChildren(...face(you, 'large').childNodes);
    presence(menuFace, yourStatus);
    menuName.textContent = you;
    menuStatus.textContent = LABEL[yourStatus];
    sign.textContent = selfState?.online ? `Sign out of ${WORKSPACE}` : `Sign in to ${WORKSPACE}`;
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
        el('span', { class: 'chatlive__presence' }, state === 'unknown' ? 'not on your screen' : state));
    }));

    // the room
    const joined = screen.rooms.includes(room);
    roomTitle.replaceChildren(el('span', { class: 'chatlive__hash', 'aria-hidden': 'true' }, '#'), room);
    for (const name of PEOPLE) for (const message of chat.screen(name).messages) if (!seen.has(message)) seen.set(message, chat.now());
    const inRoom = screen.messages.filter((message) => message.room === room);
    const here = query ? inRoom.filter((message) => message.text.toLowerCase().includes(query)) : inRoom;
    const atBottom = messages.scrollHeight - messages.scrollTop - messages.clientHeight < 40;
    const empty = !joined && !inRoom.length ? `You have not joined #${room}.`
      : query && inRoom.length ? `No messages in #${room} match “${search.value.trim()}”.`
        : 'No messages on your screen yet.';
    messages.replaceChildren(...(here.length ? here.map((message, index) => {
      const at = seen.get(message);
      const before = here[index - 1];
      const stamp = el('time', { class: 'chatlive__at' }, clock(at));
      const body = el('p', { class: 'chatlive__body' }, ...marked(message.text, query));
      if (before && before.from === message.from && at - seen.get(before) < GROUP) {
        return el('li', { class: 'chatlive__message chatlive__message--more' }, stamp, body);
      }
      return el('li', { class: 'chatlive__message' }, avatar(message.from, 'large'),
        el('p', { class: 'chatlive__line' }, el('span', { class: 'chatlive__from' }, message.from), stamp), body);
    })
      : [el('li', { class: 'chatlive__empty' }, empty)]),
      ...screen.notices.slice(-1).map((notice) => el('li', { class: 'chatlive__notice' }, notice)));
    if (atBottom) messages.scrollTop = messages.scrollHeight;
    const canPost = Boolean(selfState?.online) && joined;
    composer.hidden = !canPost;
    join.hidden = canPost || !selfState?.online;
    signedOut.hidden = Boolean(selfState?.online);
    join.textContent = `Join #${room}`;
    text.placeholder = `Message #${room}`;

    // the debugger
    if (debug.hidden) return;
    time.textContent = clock(chat.now());
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
    const chart = charts[`${inspected?.split('.')[0]}.scxml`];
    if (shown?.stored) {
      if (shownStored !== true) chart?.restart(); // nothing lit: the actor's state is in its blob
      shownStored = true;
      stateLine.textContent = `${inspected} waits in the store; its next event loads it.`;
    } else if (shown) {
      shownStored = false;
      chart?.show(shown.value);
      stateLine.replaceChildren(el('code', {}, inspected), ' is in ', el('code', {}, statePath(shown.value)));
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
