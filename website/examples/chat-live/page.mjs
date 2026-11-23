// The live chat's page: the tutorial's charts (charts/step-5 and
// charts/step-9) on @tinyactors/wasm in this tab, behind a chat window laid out as team
// chat clients are: a top bar with search, a rail of views with your avatar
// at its foot (its menu signs you in or out and switches the person you
// are), the view in a sidebar (Home: the rooms, your direct messages and the
// people, a person opening your direct message with them; DMs: your direct
// messages; Activity: what mentions you), and the room or direct
// message with its messages and composer. You are one of the people at a time; what the window shows
// is that person's screen, built only from what the charts sent to them
// (chat.mjs). A debugger pane, closed at first, holds the demo's own
// controls: the simulated clock; experiments (the charts' version, switched
// by a live migration, scripted scenarios, lost connections and faulty
// screens); any actor's chart lit in the states its host reports
// (components/live-chart.mjs); and the log of what happened.
import { createChat, elapsed as clock, GRACE, LATEST, newestFirst, PEOPLE, SCENARIOS, VERSIONS, WORKSPACE } from './chat.mjs';
import { liveChart, statePath } from '/assets/components/live-chart.mjs';

const CHARTS = ['workspace.scxml', 'room.scxml', 'user.scxml'];
const SPEEDS = [1, 10, 60];
const TICK = 250; // wall milliseconds between clock steps while it runs
const AWAY = 300_000; // the user chart's timer
const STATUS = { active: 'active', away: 'away', offline: 'offline', reconnecting: 'reconnecting' };
const LABEL = { active: 'Active', away: 'Away', offline: 'Offline', reconnecting: 'Connecting…' };
const LOSSES = [[5, '5 s'], [20, '20 s'], [120, '2 min']]; // how long a lost connection lasts: two within step 9's grace, one past it
const DROPS = [0, 0.25, 0.5, 1]; // the share of deliveries a faulty screen loses
const DELAYS = [0, 2, 10]; // seconds a faulty screen gets each delivery late
const KINDS = { input: 'Input', screen: 'Screen', lost: 'Lost', late: 'Late', fault: 'Fault', scenario: 'Scenario', version: 'Version', data: 'Data' };
const TYPED = 2; // simulated seconds between two typing notices the page sends
const GROUP = 300; // simulated seconds within which one author's messages share a header
const COLOURS = ['#c5405b', '#d2722a', '#2e8b57', '#1f7f8f', '#3567a8', '#7a4fa3', '#a8457e', '#6b7d2a', '#9a6a12']; // avatars', one picked by the name
const SVG = 'http://www.w3.org/2000/svg';
const VIEWS = { home: 'Home', dms: 'DMs', activity: 'Activity' }; // the rail's views
const count = (n) => (n > 9 ? '9+' : String(n)); // a badge's number

// The icons, drawn for this page on a 20-unit grid: [stroked paths, filled circles].
const ICONS = {
  home: [['M3.5 9 10 3.5 16.5 9v7.5a1 1 0 0 1-1 1H12v-5H8v5H4.5a1 1 0 0 1-1-1z']],
  dms: [['M2.5 4.5h10v7h-5l-3 2.5v-2.5h-2z', 'M14.5 7.5h3v7h-1.5V17l-3-2.5H9']],
  activity: [['M10 3a4.5 4.5 0 0 0-4.5 4.5v3.2L4 13.5h12l-1.5-2.8V7.5A4.5 4.5 0 0 0 10 3z', 'M8 16a2 2 0 0 0 4 0']],
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
  const name = `chatlive__face chatlive__face--${state}`;
  if (node.className !== name) node.className = name;
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
 * Keeps `list`'s children one per key, in the keys' order, made by
 * `make(key)` and brought up to date by `update(node, key)`. A node whose
 * key stays is never replaced, and moved only when the order changes: a
 * gone key's node is removed, a new key's inserted, so a button keeps its
 * focus and a message its text selection while the clock redraws the page.
 */
function keyed(list, keys, make, update) {
  const old = new Map([...list.children].map((node) => [node.dataset.key, node]));
  const wanted = new Set(keys);
  for (const [key, node] of old) if (!wanted.has(key)) node.remove();
  let next = list.firstElementChild;
  for (const key of keys) {
    let node = old.get(key);
    if (!node) {
      node = make(key);
      node.dataset.key = key;
    }
    if (node === next) next = next.nextElementSibling;
    else list.insertBefore(node, next);
    update(node, key);
  }
}

/** Sets `node`'s text only when it differs, so a redraw that changes nothing changes no node. */
function setText(node, value) {
  if (node.textContent !== value) node.textContent = value;
}

// A mention: "@" and a person's name, case-insensitive, not inside a word or an address
// ("scout@nest"); a bare name is too often a word ("the scout's trail"). One rule for
// the pills in a message and for Activity.
const MENTION = new RegExp(`(^|[^\\w@])@(${PEOPLE.join('|')})\\b`, 'gi');

/** The people `text` mentions, each as [start, end, name] of its "@name". */
function mentions(text) {
  return [...text.matchAll(MENTION)].map((found) => {
    const start = found.index + found[1].length;
    return [start, found.index + found[0].length, found[2].toLowerCase()];
  });
}

/** Whether `text` mentions `name`. */
const mentioned = (text, name) => mentions(text).some((each) => each[2] === name);

/** `text`'s characters from `start` to `end`, each case-insensitive match of `query` in the whole text marked. */
function marked(text, query, start = 0, end = text.length) {
  const parts = [];
  let at = start;
  const lower = query ? text.toLowerCase() : '';
  for (let found = query ? lower.indexOf(query) : -1; found !== -1 && found < end; found = lower.indexOf(query, found + query.length)) {
    const from = Math.max(found, at);
    const to = Math.min(found + query.length, end);
    if (to <= from) continue;
    parts.push(text.slice(at, from), el('mark', {}, text.slice(from, to)));
    at = to;
  }
  parts.push(text.slice(at, end));
  return parts.filter((part) => part !== '');
}

/**
 * A message's text: each mention of a person a pill (yours in the pill for you), with the
 * search's matches marked, across a pill's edge too. A pill is a button that opens the
 * person's card; the list's one listener opens it, so a row redrawn needs no listener of its own.
 */
function body(text, query, you) {
  const parts = [];
  let at = 0;
  for (const [start, end, name] of mentions(text)) {
    parts.push(...marked(text, query, at, start),
      el('button', { type: 'button', class: name === you ? 'chatlive__mention chatlive__mention--you' : 'chatlive__mention', 'data-person': name,
        'aria-haspopup': 'dialog', 'aria-expanded': 'false' }, ...marked(text, query, start, end)));
    at = end;
  }
  parts.push(...marked(text, query, at));
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
  // your connection is lost and its reconnect pending (or signed in with none): your user actor
  // drops what you do until it is back, so the page sends nothing and says why
  const waiting = () => chat.reconnects(you) !== null || (Boolean(selfState) && !chat.connected(you));
  // your screen's connection, which is your device's: the application's connection decides it, not
  // your actor's state. While a lost one comes back you are reconnecting, even when your actor is
  // offline to the others past the grace: a phone in a tunnel cannot sign in early.
  const link = () => (waiting() ? 'reconnecting' : chat.connected(you) ? 'connected' : 'signedOut');
  let shownStored = null; // whether the inspected actor was in the store when last drawn
  let query = ''; // the search, lowercased
  let part = 'experiments'; // the debugger's tab
  let migrating = false; // a version switch is under way
  // the newest message of each room or direct message you have seen, by "you\nroom": a newer one is
  // unread (a screen keeps a conversation's latest messages only, so counting them would stop at the cap)
  const seen = new Map();
  // Whether you read the open conversation, set at each draw: its posts are on screen (not a phone's
  // list in its place, not search results), the widget is in the window and the tab is showing.
  // Posts that come meanwhile stay unread until you look, as a client counts only what it shows.
  let reading = false;
  let inWindow = false; // the message list intersects the window
  // Every unread mark derives from it: a room's or direct message's row and badge, and Activity's.
  /**
   * The posts unread in `name` on your screen, newest first: those someone else made after the
   * message you saw last there, or after your own last post (you read up to it). None in the open
   * conversation while you read it.
   */
  const unreadPosts = (screen, name) => {
    const found = [];
    if (name === room && reading) return found;
    const last = seen.get(`${you}\n${name}`);
    for (let index = screen.messages.length - 1; index >= 0; index--) {
      const message = screen.messages[index];
      if (message.room !== name) continue;
      if (message === last || message.from === you) break;
      found.push(message);
    }
    return found;
  };
  /** Whether anything is unread in `name`, and how many of its unread posts mention you. */
  const unreadOf = (screen, name) => {
    const posts = unreadPosts(screen, name);
    return { unread: posts.length > 0, mentions: posts.filter((message) => mentioned(message.text, you)).length };
  };
  const unreadOn = (screen, name) => unreadOf(screen, name).unread;
  const chat = await createChat(tinyactors, { versions: texts, version: LATEST, onChange: () => render() });
  // The people already talking: scout and forager in general, scout's find.
  await chat.connect('scout');
  await chat.connect('forager');
  await chat.join('scout', 'general');
  await chat.join('forager', 'general');
  await chat.post('scout', 'general', 'Found sugar by the old oak. Who can carry?');

  const app = el('div', { class: 'chatlive__app', 'data-view': 'room' });

  // ---- the top bar: search, and the debugger's switch
  const search = el('input', { type: 'search', class: 'chatlive__search-field', placeholder: `Search ${WORKSPACE}`, 'aria-label': `Search ${WORKSPACE}`, maxlength: '80' });
  search.addEventListener('input', () => { query = search.value.trim().toLowerCase(); app.dataset.view = 'room'; render(); });
  search.addEventListener('keydown', (event) => { if (event.key === 'Escape' && search.value) { event.preventDefault(); clearSearch(); render(); } });
  function clearSearch() {
    search.value = '';
    query = '';
  }
  // a room's name as you see it: a channel's with its #, a direct message's as the other person
  const placeName = (name) => {
    const dm = chat.screen(you).dms.find((each) => each.room === name);
    return !dm ? `#${name}` : dm.with === you ? `${you} (you)` : dm.with;
  };
  // a result's channel or direct message, which opens it: a channel by its #name, a direct
  // message by the other person's avatar and "Direct message with …", as a client labels them
  const channelLink = (name) => {
    const dm = chat.screen(you).dms.find((each) => each.room === name);
    const label = !dm ? `#${name}` : dm.with === you ? 'Your notes' : `Direct message with ${dm.with}`;
    const link = el('button', { type: 'button', class: `chatlive__in${dm ? ' chatlive__in--dm' : ''}`, title: `Open ${placeName(name)}` },
      ...(dm ? [avatar(dm.with, 'tiny'), el('span', { class: 'chatlive__in-name' }, label)] : [label]));
    link.addEventListener('click', () => openRoom(name));
    return link;
  };
  const debugToggle = el('button', { type: 'button', class: 'chatlive__top-button', 'aria-label': 'Debugger', 'aria-pressed': 'false', title: 'Debugger and experiments' },
    icon('bug'), el('span', { class: 'chatlive__top-label' }, 'Debugger'));
  // opening it puts focus on its heading, so a keyboard is in it at once: it comes after the
  // conversation in the page's order, as a client's side panes do, which a screen reader reads that way
  debugToggle.addEventListener('click', () => {
    const open = debugToggle.getAttribute('aria-pressed') !== 'true';
    openDebugger(open);
    if (open) debugTitle.focus();
  });
  const top = el('div', { class: 'chatlive__top' },
    el('label', { class: 'chatlive__search' }, icon('search'), search),
    debugToggle);

  // ---- the rail: the workspace, the views, you. Each view takes the sidebar's place:
  // Home the channels, direct messages and people; DMs your direct messages; Activity
  // the posts that mention you and the direct messages to you. A client's More holds
  // what this chat does not have (saved items, files, canvases), so there is none.
  let railView = 'home';
  const railTabs = Object.fromEntries(Object.entries(VIEWS).map(([key, label]) => {
    const button = el('button', { type: 'button', class: 'chatlive__tab', 'data-key': key },
      el('span', { class: 'chatlive__tab-icon' }, icon(key), el('span', { class: 'chatlive__badge', 'aria-hidden': 'true', hidden: '' })),
      el('span', { class: 'chatlive__tab-label' }, label));
    if (key === railView) button.setAttribute('aria-current', 'page');
    button.addEventListener('click', () => showView(key));
    return [key, button];
  }));
  function pickView(key) {
    railView = key;
    for (const [name, button] of Object.entries(railTabs)) {
      if (name === key) button.setAttribute('aria-current', 'page');
      else button.removeAttribute('aria-current');
    }
  }
  function showView(key) {
    pickView(key);
    app.dataset.view = 'list'; // a phone shows the view in place of the conversation
    render();
  }
  const meFace = face(you, 'rail');
  const me = el('button', { type: 'button', class: 'chatlive__me', 'aria-haspopup': 'menu', 'aria-expanded': 'false', 'aria-controls': 'chatlive-menu' }, meFace);
  const rail = el('nav', { class: 'chatlive__rail', 'aria-label': 'Views' },
    el('span', { class: 'chatlive__tile', title: WORKSPACE, 'aria-hidden': 'true' }, WORKSPACE.replace(/^The /, '')[0]),
    ...Object.values(railTabs),
    me);

  // ---- your menu: who you are, signing in or out, switching person
  const menuFace = face(you, 'large');
  const menuName = el('strong', { class: 'chatlive__menu-name' });
  const menuStatus = el('span', { class: 'chatlive__menu-status' });
  const sign = el('button', { type: 'button', role: 'menuitem', tabindex: '-1', class: 'chatlive__item' });
  sign.addEventListener('click', () => { closeMenu(true); return link() === 'signedOut' ? chat.connect(you) : chat.disconnect(you); });
  const switchList = el('div', { role: 'group', 'aria-labelledby': 'chatlive-switch', class: 'chatlive__switch' },
    ...PEOPLE.map((name) => {
      const item = el('button', { type: 'button', role: 'menuitemradio', tabindex: '-1', class: 'chatlive__item chatlive__item--person', 'data-key': name },
        face(name, 'small'), el('span', { class: 'chatlive__name' }, name), el('span', { class: 'chatlive__item-note' }));
      item.addEventListener('click', () => {
        closeMenu(true);
        if (name === you) return;
        switchTo(name);
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
  // "Add channels" opens a menu, as a client's does: create a channel, or browse them all
  const addChannels = el('button', { type: 'button', class: 'chatlive__add-channels', 'aria-haspopup': 'menu', 'aria-expanded': 'false', 'aria-controls': 'chatlive-add-menu' },
    el('span', { class: 'chatlive__plus', 'aria-hidden': 'true' }, '+'), 'Add channels');
  const addItems = [['create', 'Create a new channel'], ['browse', 'Browse channels']].map(([key, label]) => {
    const item = el('button', { type: 'button', role: 'menuitem', tabindex: '-1', class: 'chatlive__item', 'data-key': key }, label);
    item.addEventListener('click', () => {
      closeAddMenu(false);
      dialogOpener = addChannels;
      return key === 'create' ? openCreate() : openBrowse();
    });
    return item;
  });
  const addMenu = el('div', { role: 'menu', id: 'chatlive-add-menu', 'aria-label': 'Add channels', class: 'chatlive__menu chatlive__add-menu', hidden: '' }, ...addItems);
  function openAddMenu() {
    const at = addChannels.getBoundingClientRect();
    const box = app.getBoundingClientRect();
    addMenu.style.left = `${Math.max(8, at.left - box.left + 8)}px`;
    addMenu.style.top = `${at.bottom - box.top + 4}px`;
    addMenu.hidden = false;
    addChannels.setAttribute('aria-expanded', 'true');
    addItems[0].focus();
  }
  function closeAddMenu(refocus) {
    if (addMenu.hidden) return;
    addMenu.hidden = true;
    addChannels.setAttribute('aria-expanded', 'false');
    if (refocus) addChannels.focus();
  }
  addChannels.addEventListener('click', () => (addMenu.hidden ? openAddMenu() : closeAddMenu(true)));
  menuKeys(addMenu, () => addItems, closeAddMenu);
  document.addEventListener('pointerdown', (event) => {
    if (!addMenu.hidden && !addMenu.contains(event.target) && !addChannels.contains(event.target)) closeAddMenu(false);
  });

  // ---- the workspace's menu, under its name, as a client's: the workspace and you, then what
  // this chat lets you do in it (browse, create, sign in or out)
  const workspaceButton = el('button', { type: 'button', class: 'chatlive__workspace-button', 'aria-haspopup': 'menu', 'aria-expanded': 'false',
    'aria-controls': 'chatlive-workspace-menu' }, WORKSPACE, icon('chevron'));
  const workspaceStatus = el('span', { class: 'chatlive__menu-status' });
  const workspaceSign = el('button', { type: 'button', role: 'menuitem', tabindex: '-1', class: 'chatlive__item' });
  workspaceSign.addEventListener('click', () => { closeWorkspaceMenu(true); return link() === 'signedOut' ? chat.connect(you) : chat.disconnect(you); });
  const workspaceItems = [['browse', 'Browse channels'], ['create', 'Create a channel']].map(([key, label]) => {
    const item = el('button', { type: 'button', role: 'menuitem', tabindex: '-1', class: 'chatlive__item', 'data-key': key }, label);
    item.addEventListener('click', () => {
      closeWorkspaceMenu(false);
      dialogOpener = workspaceButton;
      return key === 'create' ? openCreate() : openBrowse();
    });
    return item;
  });
  const workspaceMenu = el('div', { class: 'chatlive__menu chatlive__workspace-menu', hidden: '' },
    el('div', { class: 'chatlive__menu-head' }, el('span', { class: 'chatlive__tile chatlive__tile--menu', 'aria-hidden': 'true' }, WORKSPACE.replace(/^The /, '')[0]),
      el('span', { class: 'chatlive__menu-who' }, el('strong', { class: 'chatlive__menu-name' }, WORKSPACE), workspaceStatus)),
    el('div', { class: 'chatlive__rule', 'aria-hidden': 'true' }),
    el('div', { role: 'menu', id: 'chatlive-workspace-menu', 'aria-label': WORKSPACE, class: 'chatlive__menu-list' },
      ...workspaceItems, el('div', { role: 'separator', class: 'chatlive__rule' }), workspaceSign));
  const workspaceAll = () => [...workspaceItems, workspaceSign];
  function openWorkspaceMenu() {
    const at = workspaceButton.getBoundingClientRect();
    const box = app.getBoundingClientRect();
    workspaceMenu.style.left = `${Math.max(8, at.left - box.left)}px`;
    workspaceMenu.style.top = `${at.bottom - box.top + 4}px`;
    workspaceMenu.hidden = false;
    workspaceButton.setAttribute('aria-expanded', 'true');
    render();
    workspaceAll()[0].focus();
  }
  function closeWorkspaceMenu(refocus) {
    if (workspaceMenu.hidden) return;
    workspaceMenu.hidden = true;
    workspaceButton.setAttribute('aria-expanded', 'false');
    if (refocus) workspaceButton.focus();
  }
  workspaceButton.addEventListener('click', () => (workspaceMenu.hidden ? openWorkspaceMenu() : closeWorkspaceMenu(true)));
  menuKeys(workspaceMenu, workspaceAll, closeWorkspaceMenu);
  document.addEventListener('pointerdown', (event) => {
    if (!workspaceMenu.hidden && !workspaceMenu.contains(event.target) && !workspaceButton.contains(event.target)) closeWorkspaceMenu(false);
  });

  // a modal dialog: Escape and its close button close it; focus stays in it, and goes back where it came from
  const dialog = (id, title, ...body) => {
    const close = el('button', { type: 'button', class: 'chatlive__dialog-close', 'aria-label': 'Close' }, icon('close'));
    const node = el('dialog', { class: 'chatlive__dialog', 'aria-labelledby': id },
      el('div', { class: 'chatlive__dialog-head' }, el('h3', { class: 'chatlive__dialog-title', id }, title), close), ...body);
    close.addEventListener('click', () => closeDialog(node));
    node.addEventListener('keydown', (event) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        closeDialog(node);
      } else if (event.key === 'Tab') {
        // focus stays in the dialog: past its last control it comes back to the first
        const all = [...node.querySelectorAll('button, input')].filter((each) => !each.disabled && each.offsetParent !== null);
        const at = all.indexOf(document.activeElement);
        const next = event.shiftKey ? (at <= 0 ? all.length - 1 : null) : (at === all.length - 1 || at < 0 ? 0 : null);
        if (next !== null) {
          event.preventDefault();
          all[next].focus();
        }
      }
    });
    node.addEventListener('pointerdown', (event) => { if (event.target === node) closeDialog(node); }); // the backdrop
    return node;
  };
  function showDialog(node, focus) {
    node.showModal();
    focus.focus();
  }
  let dialogOpener = addChannels; // the button whose menu opened the dialog: "Add channels" or the workspace's name
  /** Closes a dialog; focus goes to `focus`, back to the button whose menu opened it, or with null where the caller puts it. */
  function closeDialog(node, focus = dialogOpener) {
    if (!node.open) return;
    node.close();
    focus?.focus();
  }

  // "Create a channel": the workspace's rule shown before the name is sent. The name is
  // lowercased and a space becomes a dash as it is typed, as a client does.
  const NAME = /^[a-z0-9][a-z0-9-]{0,31}$/; // workspace.scxml's refusal(): keep the two the same
  const NAME_MAX = 32;
  const nameField = el('input', { type: 'text', class: 'chatlive__name-field', id: 'chatlive-new-name', maxlength: String(NAME_MAX),
    autocomplete: 'off', spellcheck: 'false', placeholder: 'e.g. plan-budget', 'aria-describedby': 'chatlive-name-rule chatlive-name-problem' });
  const nameCount = el('span', { class: 'chatlive__name-count', 'aria-hidden': 'true' });
  const nameRule = el('p', { class: 'chatlive__dialog-note', id: 'chatlive-name-rule' },
    `Lowercase letters, numbers and dashes, up to ${NAME_MAX} characters. Start with a letter or number.`);
  const nameProblem = el('p', { class: 'chatlive__dialog-problem', id: 'chatlive-name-problem', 'aria-live': 'polite' });
  const createButton = el('button', { type: 'submit', class: 'chatlive__join' }, 'Create');
  const createForm = el('form', { class: 'chatlive__dialog-body' },
    el('label', { class: 'chatlive__dialog-label', for: 'chatlive-new-name' }, 'Name'),
    el('div', { class: 'chatlive__name-box' }, el('span', { class: 'chatlive__name-hash', 'aria-hidden': 'true' }, '#'), nameField, nameCount),
    nameRule, nameProblem,
    el('div', { class: 'chatlive__dialog-foot' }, createButton));
  const createDialog = dialog('chatlive-create-title', 'Create a channel', createForm);
  let refused = null; // { name, reason } of the workspace's last refusal
  let creating = false;
  /** What is wrong with the name in the field, or '' when the workspace may take it. */
  function nameProblemText() {
    const name = nameField.value;
    if (link() === 'reconnecting') return 'You’re reconnecting. Create the channel once you’re back.';
    if (link() === 'signedOut') return 'Sign in to create a channel.';
    if (!name) return '';
    if (/[^a-z0-9-]/.test(name)) return 'Use only lowercase letters, numbers and dashes.';
    if (!NAME.test(name)) return 'Start with a letter or number.';
    if (refused?.name === name) return refused.reason === 'exists' ? `#${name} already exists.` : `The workspace refused #${name}: ${refused.reason}.`;
    return '';
  }
  function drawCreate() {
    setText(nameCount, String(NAME_MAX - nameField.value.length));
    const problem = nameProblemText();
    setText(nameProblem, problem);
    nameField.setAttribute('aria-invalid', String(Boolean(problem) && Boolean(nameField.value) && link() === 'connected'));
    createButton.disabled = creating || Boolean(problem) || !nameField.value;
  }
  nameField.addEventListener('input', () => {
    const at = nameField.selectionStart;
    const value = nameField.value.toLowerCase().replace(/\s/g, '-');
    if (value !== nameField.value) {
      nameField.value = value;
      nameField.setSelectionRange(at, at);
    }
    drawCreate();
  });
  function openCreate() {
    nameField.value = '';
    refused = null;
    drawCreate();
    showDialog(createDialog, nameField);
  }
  createForm.addEventListener('submit', async (event) => {
    event.preventDefault();
    const name = nameField.value;
    if (createButton.disabled) return;
    creating = true;
    drawCreate();
    const who = you; // the person who asked: a switch during the waits does not change who acts
    try {
      const [answer, data] = await chat.createRoom(name, who);
      if (answer !== 'room.created') {
        refused = { name, reason: data.reason };
        return;
      }
      // the creator is in the channel, and sees it open
      await chat.join(who, name);
      if (who !== you) return openFor(who, name); // another person shows now, with their own dialog if any
      closeDialog(createDialog, null);
      openRoom(name);
      toAction = true; // in the composer, on a phone too: you made the channel to write in it
    } finally {
      creating = false;
      drawCreate();
    }
  });

  // "Browse channels": every channel the workspace holds, joined or not; a name opens it
  const browseList = el('ul', { class: 'chatlive__browse', 'aria-label': 'Channels' });
  const browseNote = el('p', { class: 'chatlive__dialog-note' });
  const browseDialog = dialog('chatlive-browse-title', 'Browse channels', el('div', { class: 'chatlive__dialog-body' }, browseNote, browseList));
  let browseRooms = [];
  function drawBrowse() {
    if (!browseDialog.open) return;
    const screen = chat.screen(you);
    // signed in, the list says it all: each channel, and Join or Joined beside it
    const away = link() === 'reconnecting';
    browseNote.hidden = link() === 'connected';
    setText(browseNote, away ? 'You’re reconnecting. Join a channel once you’re back.' : 'Sign in to join a channel.');
    keyed(browseList, browseRooms, (name) => {
      const open = el('button', { type: 'button', class: 'chatlive__browse-name', title: `Open #${name}` },
        el('span', { class: 'chatlive__hash', 'aria-hidden': 'true' }, '#'), el('span', { class: 'chatlive__room-name' }, name));
      open.addEventListener('click', () => { closeDialog(browseDialog, null); openRoom(name, true); });
      const joinOne = el('button', { type: 'button', class: 'chatlive__browse-join', 'aria-label': `Join #${name}` }, 'Join');
      joinOne.addEventListener('click', async () => {
        if (waiting()) return;
        const who = you;
        await chat.join(who, name);
        if (who === you && browseDialog.open) open.focus(); // the button goes once you are in
      });
      return el('li', { class: 'chatlive__browse-row' }, open, el('span', { class: 'chatlive__browse-joined' }, 'Joined'), joinOne);
    }, (node, name) => {
      const joined = screen.rooms.includes(name);
      node.children[1].hidden = !joined;
      node.children[2].hidden = joined || link() === 'signedOut';
      node.children[2].disabled = away;
    });
  }
  async function openBrowse() {
    const who = you;
    const rooms = await chat.rooms();
    if (who !== you) return; // switched person meanwhile: the dialog was the other person's
    browseRooms = [...rooms].sort();
    showDialog(browseDialog, browseDialog.querySelector('.chatlive__dialog-close'));
    drawBrowse();
    browseList.querySelector('button')?.focus();
  }
  // your direct messages, newest first, and the people: a person opens your direct message with them
  const dmList = el('ul', { class: 'chatlive__people', 'aria-label': 'Direct messages' });
  const dmSection = section('Direct messages', dmList);
  const people = el('ul', { class: 'chatlive__people', 'aria-label': 'People' });
  const peopleNote = el('p', { class: 'chatlive__note chatlive__note--people', 'aria-live': 'polite' });
  const sidebar = el('nav', { class: 'chatlive__sidebar', 'aria-label': WORKSPACE },
    el('h3', { class: 'chatlive__workspace' }, workspaceButton),
    section('Channels', roomList, addChannels),
    dmSection,
    section('People', people, peopleNote));
  /** The direct message on your screen with `name`, or undefined. */
  const dmWith = (name) => chat.screen(you).dms.find((dm) => dm.with === name);
  /**
   * Whether you can message `name` now: '' when you can, else why not. A direct message your screen
   * has always opens (to read it; while you reconnect the composer waits); a new one needs your actor
   * signed in and connected, since it asks the workspace.
   */
  function messageBlock(name) {
    if (chat.version() !== LATEST) return 'older';
    if (dmWith(name)) return '';
    if (link() === 'reconnecting') return 'waiting';
    if (link() === 'signedOut' || !selfState?.online) return 'signedOut';
    return '';
  }
  /** The sentence that says why you cannot message `name` (`block` from messageBlock). */
  function messageNote(name, block) {
    const self = name === you;
    if (block === 'waiting') return `You’re reconnecting. ${self ? 'Open your notes' : `Message ${name}`} once you’re back.`;
    return self ? 'Sign in to open your notes.' : `Sign in to message ${name}.`;
  }
  /** Opens your direct message with `name` (yourself: your notes); the workspace makes it the first time. */
  async function openDM(name, act = false) {
    peopleNote.textContent = '';
    const block = messageBlock(name);
    if (block === 'older') {
      peopleNote.textContent = `Step ${chat.version()}’s charts have no direct messages. Switch to step ${LATEST} in the debugger.`;
      return;
    }
    if (block) {
      peopleNote.textContent = messageNote(name, block);
      return;
    }
    // signed in, your actor asks the workspace; otherwise the direct message your screen has opens
    const who = you; // the person who clicked: a switch while the workspace answers does not change who opens it
    const made = selfState?.online && !waiting() ? await chat.openDM(who, name) : dmWith(name)?.room ?? null;
    if (who !== you) return made && openFor(who, made);
    if (!made) {
      peopleNote.textContent = messageNote(name, messageBlock(name) || 'signedOut');
      return;
    }
    openRoom(made, act);
  }

  // ---- the rail's other views, each in the sidebar's place
  // DMs: your direct messages, newest first, each with its last message
  const dmRows = el('ul', { class: 'chatlive__inbox-list', 'aria-label': 'Direct messages' });
  const dmEmpty = el('p', { class: 'chatlive__inbox-empty' });
  const dmView = el('section', { class: 'chatlive__sidebar chatlive__inbox', 'aria-labelledby': 'chatlive-dms', hidden: '' },
    el('div', { class: 'chatlive__inbox-head' }, el('h3', { class: 'chatlive__inbox-title', id: 'chatlive-dms' }, 'Direct messages')), dmEmpty, dmRows);
  // Activity: the posts that mention you and the direct messages to you, newest first
  const activityRows = el('ul', { class: 'chatlive__inbox-list', 'aria-label': 'Activity' });
  const activityEmpty = el('p', { class: 'chatlive__inbox-empty' });
  const markRead = el('button', { type: 'button', class: 'chatlive__mark' }, 'Mark all read');
  const activityView = el('section', { class: 'chatlive__sidebar chatlive__inbox', 'aria-labelledby': 'chatlive-activity', hidden: '' },
    el('div', { class: 'chatlive__inbox-head' }, el('h3', { class: 'chatlive__inbox-title', id: 'chatlive-activity' }, 'Activity'), markRead), activityEmpty, activityRows);
  const columns = { home: sidebar, dms: dmView, activity: activityView };
  let activityItems = new Map(); // the Activity rows' messages, by the page's key
  let picked = null; // the Activity row you opened last
  // Mark all read: each conversation Activity lists is seen up to its newest message, so its row,
  // badge and Activity's clear together. A channel with no mention of you stays bold, as in Slack,
  // whose Activity marks only its own items read.
  markRead.addEventListener('click', () => {
    const screen = chat.screen(you);
    for (const name of new Set([...activityItems.values()].map((message) => message.room))) {
      const newest = screen.messages.findLast((message) => message.room === name);
      if (newest) seen.set(`${you}\n${name}`, newest);
    }
    render();
  });

  /**
   * Shows `name`'s conversation. On a phone it takes the list's place, and focus goes to its title,
   * which a screen reader reads as the new view's name. Elsewhere, with `act`, focus goes where you
   * act in it once it is drawn: the composer, "Join channel" in a preview, or "Sign in".
   */
  function openRoom(name, act = false) {
    room = name;
    peopleNote.textContent = '';
    clearSearch();
    const narrow = app.dataset.view === 'list' && phone();
    app.dataset.view = 'room';
    toAction = act && !narrow;
    render();
    if (narrow) roomTitle.focus();
  }
  let toAction = false; // the next draw puts focus where you act in the open conversation
  /** Where you act in the open conversation: the first of these the foot shows and enables. */
  const action = () => [signIn, join, text].find((each) => !each.disabled && !each.closest('[hidden]'));
  // Each person's own view, as each has a client of their own: the conversation, the rail's
  // view, the Activity row picked, the search, the scroll position and, on a phone, whether
  // the list or the conversation shows. A switch puts away one person's and brings back the
  // other's, so nobody's posts are read because someone else had them open.
  const views = new Map();
  const firstView = () => ({ room: 'general', railView: 'home', picked: null, search: '', scroll: null, shows: 'room' });
  /**
   * After a wait, opens `name` for `who`: on the screen when `who` is still the person shown,
   * else in `who`'s saved view, so they find it open when switched back.
   */
  function openFor(who, name) {
    if (who === you) return openRoom(name);
    views.set(who, { ...(views.get(who) ?? firstView()), room: name, search: '', scroll: null, shows: 'room' });
  }
  let scrollTo; // the scroll position the next draw restores: a number, null for the end, undefined for none
  function switchTo(name) {
    const end = messages.scrollHeight - messages.scrollTop - messages.clientHeight < 40;
    views.set(you, { room, railView, picked, search: search.value, scroll: end ? null : messages.scrollTop, shows: app.dataset.view });
    const view = views.get(name) ?? firstView();
    you = name;
    room = view.room;
    picked = view.picked;
    search.value = view.search;
    query = view.search.trim().toLowerCase();
    scrollTo = view.scroll;
    app.dataset.view = view.shows;
    pickView(view.railView);
    peopleNote.textContent = '';
    inspect(`user.${you}`);
  }
  /** A phone shows the list or the conversation, with a back button. */
  const phone = () => getComputedStyle(back).display !== 'none';
  /** Whether the open conversation's posts are where you can read them now (`reading`). */
  const readable = () => inWindow && !document.hidden && !query && !(app.dataset.view === 'list' && phone());
  /** Back from a conversation to the view's list, focus on the row you came from. */
  function goBack() {
    app.dataset.view = 'list';
    (columns[railView].querySelector('[aria-current="true"]') ?? railTabs[railView]).focus();
  }

  // ---- the room you look at
  const roomTitle = el('h3', { class: 'chatlive__title', tabindex: '-1' });
  const titleFace = face(you, 'small');
  const back = el('button', { type: 'button', class: 'chatlive__back', 'aria-label': 'Back to Home' }, icon('back'));
  back.addEventListener('click', goBack);
  const banner = el('p', { class: 'chatlive__banner', role: 'status', hidden: '' },
    el('strong', {}, 'Connecting…'), ` The others see you as away, and new messages wait ${GRACE} seconds for you.`);
  const messages = el('ol', { class: 'chatlive__messages', 'aria-label': 'Messages' });
  // the posts are read when you can see them: the list comes into the window (or a phone's list gives
  // way to it: a hidden list leaves the window), or the tab shows again
  new IntersectionObserver((entries) => { inWindow = entries.at(-1).isIntersecting; render(); }).observe(messages);
  document.addEventListener('visibilitychange', () => render());
  messages.addEventListener('click', (event) => {
    const pill = event.target.closest('.chatlive__mention');
    if (!pill) return;
    if (pill === cardPill) closeCard(true);
    else openCard(pill);
  });
  messages.addEventListener('scroll', () => placeCard());
  // a conversation's earlier posts, read from the store; the view keeps its place below them
  const loadEarlier = el('button', { type: 'button', class: 'chatlive__earlier-button' }, 'Load earlier messages');
  let fromEnd; // the distance from the end the next draw keeps, after earlier posts went in above
  loadEarlier.addEventListener('click', async () => {
    fromEnd = messages.scrollHeight - messages.scrollTop;
    if (await chat.loadEarlier(you, room) === 0) fromEnd = undefined;
  });
  // ---- a person's card, opened from an @mention as a client's is: their avatar, name and
  // presence as your screen knows them, and "Message", which opens your direct message with them
  const cardAvatar = el('span', {});
  const cardName = el('strong', { class: 'chatlive__profile-name', id: 'chatlive-profile-name' });
  const cardStatus = el('span', { class: 'chatlive__profile-status' }, el('span', { class: 'chatlive__dot', 'aria-hidden': 'true' }), el('span', {}));
  const cardMessage = el('button', { type: 'button', class: 'chatlive__profile-message' }, icon('dms'), 'Message');
  const cardNote = el('p', { class: 'chatlive__profile-note' });
  const card = el('div', { class: 'chatlive__profile', role: 'dialog', 'aria-labelledby': 'chatlive-profile-name', tabindex: '-1', hidden: '' },
    el('div', { class: 'chatlive__profile-head' }, cardAvatar, el('div', { class: 'chatlive__profile-who' }, cardName, cardStatus)), cardMessage, cardNote);
  let cardPill = null; // the pill whose card is open
  let cardView = null; // the view it opened in: another conversation, person or search closes it
  const cardViewNow = () => [you, room, query, railView, app.dataset.view].join('\n');
  function openCard(pill) {
    closeCard(false);
    peopleNote.textContent = ''; // the card answers for the person now: one note at a time says why you cannot message
    cardPill = pill;
    cardView = cardViewNow();
    pill.setAttribute('aria-expanded', 'true');
    card.hidden = false;
    drawCard();
    placeCard();
    (cardMessage.hidden ? card : cardMessage).focus();
  }
  function closeCard(refocus) {
    if (!cardPill) return;
    const pill = cardPill;
    cardPill = null;
    card.hidden = true;
    pill.setAttribute('aria-expanded', 'false');
    if (refocus && pill.isConnected) pill.focus();
  }
  /** The card under its pill, or above it when there is no room below; on a phone the stylesheet puts it at the foot. */
  function placeCard() {
    if (!cardPill) return;
    if (phone()) { card.style.left = card.style.top = ''; return; }
    const at = cardPill.getBoundingClientRect();
    const box = app.getBoundingClientRect();
    const left = Math.min(Math.max(8, at.left - box.left), box.width - card.offsetWidth - 8);
    const below = at.bottom - box.top + 4;
    const top = below + card.offsetHeight > box.height - 8 ? at.top - box.top - card.offsetHeight - 4 : below;
    card.style.left = `${left}px`;
    card.style.top = `${Math.max(8, top)}px`;
  }
  /** The open card's person as your screen knows them now. */
  function drawCard() {
    if (!cardPill) return;
    const name = cardPill.dataset.person;
    const self = name === you;
    if (cardAvatar.dataset.who !== name) {
      cardAvatar.dataset.who = name;
      cardAvatar.replaceChildren(avatar(name, 'card'));
    }
    if (cardName.dataset.who !== `${name}\n${self}`) {
      cardName.dataset.who = `${name}\n${self}`;
      cardName.replaceChildren(name, ...(self ? [el('span', { class: 'chatlive__profile-you' }, ' (you)')] : []));
    }
    const state = self ? yourStatusNow() : STATUS[chat.screen(you).roster[name]] ?? 'unknown';
    const className = `chatlive__profile-status chatlive__profile-status--${state}`;
    if (cardStatus.className !== className) cardStatus.className = className;
    cardStatus.hidden = state === 'unknown';
    setText(cardStatus.lastChild, LABEL[state] ?? '');
    // step 5 has no direct messages; signed out or reconnecting, only one your screen has still opens
    const block = messageBlock(name);
    if (block && document.activeElement === cardMessage) card.focus();
    cardMessage.hidden = Boolean(block);
    cardMessage.title = self ? 'Open your notes' : `Message ${name}`;
    setText(cardNote, block === 'older' ? `Step ${chat.version()}’s charts have no direct messages.` : block ? messageNote(name, block) : '');
    cardNote.hidden = !block;
  }
  cardMessage.addEventListener('click', () => {
    const name = cardPill.dataset.person;
    closeCard(false);
    openDM(name, true);
  });
  card.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') {
      event.preventDefault();
      closeCard(true);
    } else if (event.key === 'Tab') {
      // Tab goes through the card; past its edge the card closes and focus is back on the pill
      const all = [...card.querySelectorAll('button')].filter((each) => !each.hidden);
      const at = all.indexOf(document.activeElement);
      if (event.shiftKey ? at <= 0 : at === all.length - 1) {
        event.preventDefault();
        closeCard(true);
      }
    }
  });
  document.addEventListener('pointerdown', (event) => {
    if (!cardPill || card.contains(event.target) || cardPill.contains(event.target)) return;
    const pill = cardPill;
    closeCard(false);
    // a click on nothing that takes focus leaves it on the pill, as Escape does
    setTimeout(() => { if ((!document.activeElement || document.activeElement === document.body) && pill.isConnected) pill.focus({ preventScroll: true }); });
  });

  // the list is updated in place; this region alone announces each message that reaches the screen
  const arrived = el('p', { class: 'visually-hidden', 'aria-live': 'polite' });
  const text = el('input', { type: 'text', class: 'chatlive__text', 'aria-label': 'Message', maxlength: '200' });
  const send = el('button', { type: 'submit', class: 'chatlive__send', 'aria-label': 'Send' }, arrow());
  const join = el('button', { type: 'button', class: 'chatlive__join' }, 'Join channel');
  join.addEventListener('click', () => waiting() || chat.join(you, room));
  // a channel you have not joined: its posts from the store, and this bar in the composer's place
  const viewingName = el('strong', {});
  const viewing = el('div', { class: 'chatlive__signed-out chatlive__viewing' }, el('p', {}, 'You are viewing ', viewingName), join);
  const signIn = el('button', { type: 'button', class: 'chatlive__join' }, 'Sign in');
  signIn.addEventListener('click', () => chat.connect(you));
  const signedOut = el('div', { class: 'chatlive__signed-out' }, el('p', {}, 'You’re signed out. Sign in to send messages.'), signIn);
  const composer = el('form', { class: 'chatlive__composer' }, text, send);
  const typingLine = el('p', { class: 'chatlive__typing', 'aria-live': 'polite' });
  let typed = -Infinity; // the simulated time of the last typing notice sent
  // one draft per person and room, as Slack keeps one per conversation
  const drafts = new Map();
  let drafted = `${you}\n${room}`; // whose draft the composer holds
  function swapDraft() {
    const key = `${you}\n${room}`;
    if (key === drafted) return;
    if (text.value) drafts.set(drafted, text.value);
    else drafts.delete(drafted);
    drafted = key;
    text.value = drafts.get(key) ?? '';
    send.classList.toggle('chatlive__send--ready', text.value.trim() !== '');
    typed = -Infinity; // the new person's first key tells the room, as usual
  }
  text.addEventListener('input', () => {
    send.classList.toggle('chatlive__send--ready', text.value.trim() !== '');
    if (!selfState?.online || waiting() || chat.now() - typed < TYPED) return; // the chart throttles too: one notice to the room per 3 s
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
  const foot = el('div', { class: 'chatlive__foot' }, composer, viewing, signedOut, typingLine);
  const main = el('section', { class: 'chatlive__room', 'aria-label': 'Channel' }, el('header', { class: 'chatlive__bar-title' }, back, roomTitle), banner, messages, arrived, foot);

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
      // step 5's charts have no direct messages: one open goes back to general
      if (step !== LATEST && chat.screen(you).dms.some((dm) => dm.room === room)) room = 'general';
      peopleNote.textContent = '';
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
        run.disabled = true; // now: the next draw waits for the start to go through
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

  const within = LOSSES.filter(([seconds]) => seconds < GRACE).map(([, label]) => label);
  const past = LOSSES.filter(([seconds]) => seconds > GRACE).map(([, label]) => label);
  const experiments = el('div', { class: 'chatlive__panel', role: 'tabpanel', id: 'chatlive-experiments', 'aria-labelledby': 'chatlive-tab-experiments' },
    el('h4', { class: 'chatlive__subheading' }, 'Charts'), versionPick, versionAbout,
    el('p', { class: 'chatlive__hint' }, 'A switch migrates every running actor to the other charts, without a restart.'), versionNote,
    el('h4', { class: 'chatlive__subheading' }, 'Scenarios'), scenarioList,
    el('h4', { class: 'chatlive__subheading' }, 'People'),
    el('p', { class: 'chatlive__hint' }, `Step ${LATEST} shows a lost connection as away at once and offline after ${GRACE} s: ${within.join(' and ')} come back in time, ${past.join(' and ')} does not.`),
    personList);

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
  const debugTitle = el('h3', { class: 'chatlive__title', tabindex: '-1' }, 'Debugger');
  const debug = el('aside', { class: 'chatlive__debug', 'aria-label': 'Debugger', hidden: '' },
    el('header', { class: 'chatlive__bar-title' }, debugTitle, closeDebugger),
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
  // Escape in the debugger closes it, as its close button does
  debug.addEventListener('keydown', (event) => {
    if (event.key !== 'Escape' || event.defaultPrevented) return;
    event.preventDefault();
    openDebugger(false);
    debugToggle.focus();
  });
  function openDebugger(open) {
    debug.hidden = !open;
    debugToggle.setAttribute('aria-pressed', String(open));
    app.classList.toggle('chatlive__app--debug', open);
    if (open) { logDrawn = ''; showChart(); }
    render();
  }

  app.append(top, rail, sidebar, dmView, activityView, el('div', { class: 'chatlive__pane' }, main, debug), menu, addMenu, workspaceMenu, card, createDialog, browseDialog);
  // Escape on a phone goes back from a conversation to its list, as the back button does
  app.addEventListener('keydown', (event) => {
    if (event.key !== 'Escape' || event.defaultPrevented || app.dataset.view !== 'room' || !debug.hidden || !menu.hidden || !workspaceMenu.hidden || !phone()) return;
    if (event.target.matches('input') && event.target.value) return;
    event.preventDefault();
    goBack();
  });
  root.replaceChildren(app, status);
  status.textContent = '';
  ready = true;

  // ---- what each part shows, after every change
  // Your actor's state value. While it is in the store (its state is in
  // its blob), what the workspace last told another person online about
  // you; with nobody online to tell, the last state read. A lost
  // connection on the newest charts shows as away to the others, so while the
  // agenda still holds your reconnect, the last state read (reconnecting) stands.
  const known = new Map();
  function yours(self) {
    if (!self.stored) { known.set(you, self.value); return asSelf(self.value); }
    const last = known.get(you) ?? null;
    const mine = chat.screen(you).roster;
    const told = PEOPLE.filter((name) => name !== you && ['active', 'away'].includes(mine[name]))
      .map((name) => chat.screen(name).roster[you]).find(Boolean);
    if (told === 'offline') return null;
    if (last === 'reconnecting' && chat.reconnects(you) !== null && chat.version() === LATEST) return last;
    if (!told) return asSelf(last);
    return { online: told };
  }
  const asSelf = (value) => (value === 'offline' || value === null ? null : value);
  // the page's own key for each message it shows: a numbered post's is its room and the store's
  // number, so a post previewed and then on your screen after a join is one row; an unnumbered
  // one's (step 5's) is its object, which stays the same while on the screen
  const messageKeys = new WeakMap();
  let counted = 0;
  const keyOf = (message) => {
    if (message.seq !== undefined) return `s${message.room}\n${message.seq}`;
    if (!messageKeys.has(message)) messageKeys.set(message, `m${++counted}`);
    return messageKeys.get(message);
  };
  const announced = new Set();
  let announcedView = null;
  const statusOf = (state) => (state === 'reconnecting' ? 'reconnecting' : state?.online ?? 'offline');
  /** Your status on your own screen: your connection's while it is lost or closed, else your actor's. */
  const yourStatusNow = () => ({ reconnecting: 'reconnecting', signedOut: 'offline' })[link()] ?? statusOf(selfState);

  /** The top of a conversation: a channel's name and who created it, or the person a direct message is with. */
  function drawBeginning(node, dm, name) {
    const made = dm ? null : chat.created(name);
    const drawn = dm ? `dm\n${dm.with}\n${you}` : `room\n${name}\n${made?.by}\n${made?.at}\n${you}`;
    if (node.drawn === drawn) return;
    node.drawn = drawn;
    if (dm) {
      const notes = dm.with === you;
      node.replaceChildren(avatar(dm.with, 'begin'),
        el('p', { class: 'chatlive__begin-title' }, dm.with, ...(notes ? [el('span', { class: 'chatlive__title-note' }, ' (you)')] : [])),
        el('p', { class: 'chatlive__begin-text' }, notes ? 'This is your space. Only you see what you post here.'
          : `This conversation is just between you and ${dm.with}.`));
      return;
    }
    const who = !made ? '' : made.by === null ? ' The workspace created it at the start.'
      : ` ${made.by === you ? 'You' : made.by} created it at ${clock(made.at)}.`;
    node.replaceChildren(el('p', { class: 'chatlive__begin-title' }, el('span', { class: 'chatlive__hash', 'aria-hidden': 'true' }, '#'), name),
      el('p', { class: 'chatlive__begin-text' }, `This is the very beginning of the #${name} channel.${who}`));
  }

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

  // The page state a draw depends on: who you are, the open conversation, the search, the rail's
  // view and the debugger's selections. A click can change it while a draw waits for the host.
  const viewOf = () => [you, room, query, search.value, railView, inspected, part, debug.hidden, kindPick.value, picked].join('\n');

  async function draw() {
    swapDraft();
    const drawing = viewOf();
    const showActors = !debug.hidden && part === 'actors';
    const [actors, stats, self, shown] = await Promise.all([chat.actors(), showActors ? chat.stats() : null, chat.state(`user.${you}`),
      inspected && showActors ? chat.state(inspected) : null]);
    // the state changed during the wait: what was read is another view's (another person's
    // actor, screen or chart), so this draw shows nothing, and render() draws again. Past this
    // check the draw does not wait, so the page state it reads is the state `drawing` recorded.
    if (viewOf() !== drawing) {
      again = true;
      return;
    }
    const screen = chat.screen(you);
    reading = readable();
    selfState = yours(self);
    const yourStatus = yourStatusNow();
    const reconnecting = yourStatus === 'reconnecting';
    const cut = reconnecting; // what you do now would be dropped: the composer and Join wait
    const roster = { ...screen.roster, [you]: yourStatus };

    // you, on the rail and in your menu
    if (meFace.dataset.who !== you) {
      meFace.dataset.who = menuFace.dataset.who = you;
      meFace.replaceChildren(...face(you, 'rail').childNodes);
      menuFace.replaceChildren(...face(you, 'large').childNodes);
    }
    presence(meFace, yourStatus);
    me.setAttribute('aria-label', `${you}, ${LABEL[yourStatus]}`);
    presence(menuFace, yourStatus);
    setText(menuName, you);
    setText(menuStatus, LABEL[yourStatus]);
    setText(sign, link() === 'signedOut' ? `Sign in to ${WORKSPACE}` : `Sign out of ${WORKSPACE}`);
    setText(workspaceSign, sign.textContent);
    setText(workspaceStatus, `${you} · ${LABEL[yourStatus]}`);
    for (const item of switchList.children) {
      const name = item.dataset.key;
      const state = STATUS[roster[name]] ?? 'unknown';
      item.setAttribute('aria-checked', String(name === you));
      presence(item.firstChild, state);
      setText(item.lastChild, name === you ? 'you' : state === 'unknown' ? '' : state);
    }

    // the sidebar
    const rooms = actors.filter((actor) => actor.chart === 'room' && !actor.direct).map((actor) => actor.name.slice(5));
    // an open dialog follows the chat: signing in or out, a join, a channel someone else made
    if (createDialog.open) drawCreate();
    if (browseDialog.open) {
      browseRooms = [...new Set([...browseRooms, ...rooms])].sort();
      drawBrowse();
    }
    // a direct message of the other person's (after a person switch) is not yours: back to general
    const dms = chat.version() === LATEST ? chat.directMessages(you) : [];
    const yourDM = screen.dms.find((dm) => dm.room === room);
    if (!rooms.includes(room) && !(yourDM && chat.version() === LATEST)) room = 'general';
    const dm = chat.version() === LATEST ? screen.dms.find((each) => each.room === room) : undefined;

    // the channels you joined, and a channel you preview (from Browse) while it is open, as a client lists them
    const listed = rooms.filter((name) => screen.rooms.includes(name) || (name === room && !dm));
    keyed(roomList, listed, (name) => {
      // a long name ends in an ellipsis, the whole of it in the title
      const button = el('button', { type: 'button', class: 'chatlive__room-link', title: `#${name}` },
        el('span', { class: 'chatlive__hash', 'aria-hidden': 'true' }, '#'), el('span', { class: 'chatlive__room-name' }, name),
        el('span', { class: 'chatlive__room-note' }), el('span', { class: 'chatlive__room-count', 'aria-hidden': 'true', hidden: '' }));
      // opening it puts focus where you act in it, as Browse and Activity do (on a phone: its title)
      button.addEventListener('click', () => openRoom(name, true));
      return el('li', {}, button);
    }, (node, name) => {
      const button = node.firstChild;
      const joinedHere = screen.rooms.includes(name);
      // a joined channel with posts you have not seen is bold, a mention of you counted on it, as a client shows them
      const { unread, mentions } = joinedHere ? unreadOf(screen, name) : { unread: false, mentions: 0 };
      button.setAttribute('aria-current', String(name === room));
      button.classList.toggle('chatlive__room-link--preview', !joinedHere);
      button.classList.toggle('chatlive__room-link--unread', unread);
      const [, , note, mark] = button.children;
      setText(note, !joinedHere ? ', not joined' : unread ? `, unread${mentions ? `, ${mentions} ${mentions === 1 ? 'mention' : 'mentions'}` : ''}` : '');
      mark.hidden = !mentions;
      setText(mark, mentions ? count(mentions) : '');
    });
    // a person: avatar, presence, name; `note` says more to a screen reader. One button, which opens the direct message
    const personButton = (name, open) => {
      const button = el('button', { type: 'button', class: 'chatlive__person' }, face(name, 'small'), el('span', { class: 'chatlive__name' }, name),
        el('span', { class: 'chatlive__you-mark' }, 'you'), el('span', { class: 'chatlive__presence' }));
      button.addEventListener('click', open);
      return el('li', {}, button);
    };
    const drawPerson = (button, name, state, current, extra = '') => {
      const className = `chatlive__person chatlive__person--${state}${extra}`;
      if (button.className !== className) button.className = className;
      presence(button.firstChild, state);
      button.children[2].hidden = name !== you;
      button.setAttribute('aria-current', String(current));
    };
    dmSection.hidden = chat.version() !== LATEST;
    // keyed by the room and the other person: switching person keeps no row of someone else's
    const dmKey = (each) => `${each.room}\n${each.with}`;
    keyed(dmList, dms.map(dmKey), (key) => personButton(key.split('\n')[1],
      () => openRoom(key.split('\n')[0], true)), (node, key) => {
      const each = dms.find((one) => dmKey(one) === key);
      const unread = unreadOn(screen, each.room);
      const state = each.with === you ? yourStatus : STATUS[roster[each.with]] ?? 'unknown';
      drawPerson(node.firstChild, each.with, state, !query && each.room === room, unread ? ' chatlive__person--unread' : '');
      setText(node.firstChild.lastChild, `${state === 'unknown' ? '' : LABEL[state].toLowerCase()}${unread ? ', unread messages' : ''}`);
      node.firstChild.title = `Message ${each.with}`;
    });
    keyed(people, PEOPLE, (name) => personButton(name, () => openDM(name, true)), (node, name) => {
      const state = STATUS[roster[name]] ?? 'unknown';
      drawPerson(node.firstChild, name, state, false);
      node.firstChild.title = chat.version() === LATEST ? `Message ${name}` : name;
      setText(node.firstChild.lastChild, state === 'unknown' ? 'not on your screen' : LABEL[state].toLowerCase());
    });

    // the room
    const joined = screen.rooms.includes(room);
    // a search looks through every channel on your screen, as a client searches its workspace
    const searched = search.value.trim();
    const heading = query ? `search\n${searched}` : `room\n${room}\n${dm?.with === you ? 'you' : dm?.with}`;
    const newHeading = roomTitle.dataset.heading !== heading;
    if (newHeading) {
      roomTitle.dataset.heading = heading;
      // a direct message's title is the other person, with their presence, as a client's is
      if (query) roomTitle.replaceChildren(`Results for “${searched}”`);
      else if (dm) roomTitle.replaceChildren(titleFace, dm.with, ...(dm.with === you ? [el('span', { class: 'chatlive__title-note' }, ' (you)')] : []));
      else roomTitle.replaceChildren(el('span', { class: 'chatlive__hash', 'aria-hidden': 'true' }, '#'), room);
      roomTitle.title = query ? `Results for “${searched}”` : placeName(room);
      if (dm && titleFace.dataset.who !== dm.with) {
        titleFace.dataset.who = dm.with;
        titleFace.replaceChildren(...face(dm.with, 'small').childNodes);
      }
    }
    if (dm) presence(titleFace, dm.with === you ? yourStatus : STATUS[roster[dm.with]] ?? 'unknown');
    main.setAttribute('aria-label', dm ? 'Direct message' : 'Channel');
    const connected = link() !== 'signedOut';
    // a channel you have not joined shows its latest posts from the store (a preview): they are
    // not on your screen, so search and Activity, which read your screen, leave them out
    const preview = connected && !joined && !dm ? chat.preview(room) : null;
    const inRoom = preview ?? screen.messages.filter((message) => message.room === room);
    // search results newest first, as a client lists them sorted by date
    const here = query ? screen.messages.filter((message) => message.text.toLowerCase().includes(query)).sort(newestFirst) : inRoom;
    const atBottom = messages.scrollHeight - messages.scrollTop - messages.clientHeight < 40;
    // the conversation's beginning, above its first post, as a client shows it: only where the
    // store's numbers say the oldest post shown is the first (step 5's posts have none)
    const beginning = !query && chat.version() === LATEST && Boolean(dm || joined || preview) && (!inRoom.length || inRoom[0].seq === 1);
    // a screen keeps a conversation's latest posts: where older ones are in the store, a button reads them
    const earlier = !query && !preview && chat.version() === LATEST && Boolean(dm || joined) && inRoom[0]?.seq > 1;
    const empty = query ? `No messages on your screen match “${searched}”.`
      : !joined && connected && !preview ? `Step ${chat.version()}’s charts keep no history. Join #${room} to see its new messages.`
        : !joined && !inRoom.length ? `You have not joined #${room}.`
          : 'No messages on your screen yet.';
    // one row per message, kept while the message stays on the screen; rebuilt only when its
    // header, time, the search's highlight or its mention of you changes
    const rows = new Map(here.map((message, index) => [keyOf(message), [message, here[index - 1]]]));
    const notice = screen.notices.at(-1);
    const keys = [...(beginning ? ['beginning'] : []), ...(earlier ? ['earlier'] : []), ...(here.length ? rows.keys() : beginning ? [] : ['empty']), ...(notice ? ['notice'] : [])];
    const ROW = { empty: 'chatlive__empty', notice: 'chatlive__notice', beginning: 'chatlive__beginning', earlier: 'chatlive__earlier' };
    const focusEarlier = document.activeElement === loadEarlier;
    keyed(messages, keys, (key) => key === 'earlier' ? el('li', { class: ROW.earlier }, loadEarlier) : el('li', { class: ROW[key] ?? 'chatlive__message' }), (node, key) => {
      if (key === 'earlier') return;
      if (key === 'empty') return setText(node, empty);
      if (key === 'beginning') return drawBeginning(node, dm, room);
      if (key === 'notice') return setText(node, notice);
      const [message, before] = rows.get(key);
      // search results stand alone, each labelled with its channel
      const more = Boolean(!query && before && before.from === message.from && Math.abs(message.at - before.at) < GROUP);
      // switching person redraws only the rows that mention the person you were or are
      const toYou = mentioned(message.text, you);
      const drawn = `${more}\n${message.at}\n${query}\n${toYou ? you : ''}\n${message.text}`;
      if (node.drawn === drawn) return;
      node.drawn = drawn;
      // when the post was made (step 9's store records it), or on step 5 when it reached this screen
      const stamp = el('time', { class: 'chatlive__at' }, clock(message.at));
      const paragraph = el('p', { class: 'chatlive__body' }, ...body(message.text, query, you));
      // a post someone else made that mentions you is tinted, as Activity lists it
      node.className = `chatlive__message${more ? ' chatlive__message--more' : ''}${toYou && message.from !== you ? ' chatlive__message--you' : ''}`;
      if (more) node.replaceChildren(stamp, paragraph);
      else node.replaceChildren(avatar(message.from, 'large'), el('p', { class: 'chatlive__line' }, el('span', { class: 'chatlive__from' }, message.from), stamp,
        ...(query ? [channelLink(message.room)] : [])), paragraph);
    });
    // the card closes with its conversation, and with its pill when a redraw replaced the row
    if (cardPill && (cardView !== cardViewNow() || !cardPill.isConnected)) {
      const inside = card.contains(document.activeElement);
      closeCard(false);
      if (inside) roomTitle.focus();
    }
    if (fromEnd !== undefined) {
      messages.scrollTop = messages.scrollHeight - fromEnd; // earlier posts above: what you read stays in place
      fromEnd = undefined;
    } else if (scrollTo !== undefined) {
      messages.scrollTop = scrollTo ?? messages.scrollHeight; // a person's view brought back where they left it
      scrollTo = undefined;
    } else if (query) {
      if (newHeading) messages.scrollTop = 0; // search results start at the newest, on top
    } else if (atBottom) messages.scrollTop = messages.scrollHeight;
    // the button gone (post 1 is on screen): focus goes to the conversation's title
    if (focusEarlier && !earlier) roomTitle.focus();
    if (reading && !preview) seen.set(`${you}\n${room}`, inRoom.at(-1)); // what you can read now is read
    // the live region says each message new to this screen and room, not those there when you came
    const view = `${you}\n${room}`;
    const fresh = inRoom.filter((message) => !announced.has(keyOf(message)));
    for (const message of fresh) announced.add(keyOf(message));
    if (view !== announcedView) {
      announcedView = view;
      announced.clear();
      for (const message of inRoom) announced.add(keyOf(message));
      setText(arrived, '');
    } else if (fresh.length) {
      setText(arrived, fresh.filter((message) => message.from !== you).map((message) => `${message.from}: ${message.text}`).join('\n'));
    }
    // focus on "Sign in", "Join channel" or the composer follows what takes its place: joining puts you in the composer
    const acting = toAction || [signIn, join, text].includes(document.activeElement);
    banner.hidden = !reconnecting;
    // past the grace (or on step 5, at once) the others see you offline; step 9 catches up from the store
    setText(banner.lastChild, chat.version() !== LATEST ? ' The others see you as offline until you’re back.'
      : selfState ? ` The others see you as away, and new messages wait ${GRACE} seconds for you.`
        : ' The others now see you as offline. You’ll get the messages from meanwhile when you’re back.');
    foot.hidden = Boolean(query); // a list of results has no composer
    composer.hidden = !(connected && joined);
    composer.classList.toggle('chatlive__composer--off', cut);
    text.disabled = cut;
    send.disabled = cut;
    viewing.hidden = !connected || joined;
    setText(viewingName, `#${room}`);
    join.disabled = cut;
    signedOut.hidden = connected;
    text.placeholder = cut ? 'Connecting…' : dm?.with === you ? 'Jot something down' : `Message ${dm ? dm.with : `#${room}`}`;
    if (acting) {
      // nothing to act on (a lost connection disables the composer): a conversation just opened shows its title
      const next = action() ?? (toAction ? roomTitle : null);
      if (next && document.activeElement !== next) next.focus();
      toAction = false;
    }
    // step 5's charts send no typing notices, and no typing.stopped for those the newest sent before a switch
    const typers = connected && chat.version() === LATEST ? (screen.typing[room] ?? []).filter((name) => name !== you) : [];
    setText(typingLine, typers.length === 0 ? ''
      : typers.length === 1 ? `${typers[0]} is typing…`
        : typers.length === 2 ? `${typers[0]} and ${typers[1]} are typing…`
          : 'Several people are typing…');

    drawCard();
    placeCard();
    // the rail's views: the one picked shows in the sidebar's place
    for (const [key, column] of Object.entries(columns)) column.hidden = key !== railView;
    back.setAttribute('aria-label', `Back to ${VIEWS[railView]}`);
    const unreadIn = (each) => unreadOn(screen, each.room);
    const badge = (key, n, what) => {
      const mark = railTabs[key].querySelector('.chatlive__badge');
      mark.hidden = !n;
      setText(mark, count(n));
      if (n) railTabs[key].setAttribute('aria-label', `${VIEWS[key]}, ${n} ${what}`);
      else railTabs[key].removeAttribute('aria-label');
    };
    badge('dms', dms.filter(unreadIn).length, 'unread');
    // DMs: on step 5 there are none to list
    setText(dmEmpty, chat.version() !== LATEST ? `Step ${chat.version()}’s charts have no direct messages. Switch to step ${LATEST} in the debugger.`
      : dms.length ? '' : 'No direct messages yet. Click a person in Home to start one.');
    keyed(dmRows, dms.map(dmKey), (key) => {
      const [dmRoom, name] = key.split('\n');
      const button = el('button', { type: 'button', class: 'chatlive__row chatlive__row--dm' }, face(name, 'large'),
        el('span', { class: 'chatlive__row-name' }, name === you ? `${you} (you)` : name), el('time', { class: 'chatlive__row-at' }),
        el('span', { class: 'chatlive__row-text' }), el('span', { class: 'visually-hidden' }));
      button.addEventListener('click', () => openRoom(dmRoom, true));
      return el('li', {}, button);
    }, (node, key) => {
      const each = dms.find((one) => dmKey(one) === key);
      const button = node.firstChild;
      const state = each.with === you ? yourStatus : STATUS[roster[each.with]] ?? 'unknown';
      const unread = unreadIn(each);
      presence(button.firstChild, state);
      button.classList.toggle('chatlive__row--unread', unread);
      button.setAttribute('aria-current', String(!query && each.room === room));
      const [, , at, last, note] = button.children;
      setText(at, each.last ? clock(each.last.at) : '');
      setText(last, each.last ? `${each.last.from === you ? 'You: ' : ''}${each.last.text}` : 'No messages yet');
      setText(note, `${state === 'unknown' ? '' : `, ${LABEL[state].toLowerCase()}`}${unread ? ', unread' : ''}`);
    });
    // Activity: a post that mentions you by "@name" (a bare name is too often a word: "the scout's trail"),
    // and each post someone else made in a direct message with you; newest first
    const direct = new Set(screen.dms.map((each) => each.room));
    const items = screen.messages.filter((message) => message.from !== you
      && (direct.has(message.room) ? chat.version() === LATEST : mentioned(message.text, you)))
      .sort(newestFirst);
    activityItems = new Map(items.map((message) => [keyOf(message), message]));
    // the row current while its conversation is open, however it was opened: the one you
    // picked there, else the conversation's newest
    const current = query ? null
      : picked && activityItems.get(picked)?.room === room ? picked
        : [...activityItems].find(([, message]) => message.room === room)?.[0] ?? null;
    // an item is unread while its conversation is: the same record as the rows' and badges'
    const unreadItems = new Set([...new Set(items.map((message) => message.room))].flatMap((name) => unreadPosts(screen, name)));
    badge('activity', items.filter((message) => unreadItems.has(message)).length, 'unread');
    markRead.hidden = !items.some((message) => unreadItems.has(message));
    setText(activityEmpty, items.length ? '' : `Posts that mention @${you} and direct messages to you show here.`);
    keyed(activityRows, [...activityItems.keys()], (key) => {
      const message = activityItems.get(key);
      const where = direct.has(message.room) ? 'Direct message' : `Mention in #${message.room}`;
      const button = el('button', { type: 'button', class: 'chatlive__row chatlive__row--activity' },
        el('span', { class: 'chatlive__row-where' }, el('span', { 'aria-hidden': 'true' }, direct.has(message.room) ? '' : '@ '), where),
        el('time', { class: 'chatlive__row-at' }, clock(message.at)), avatar(message.from, 'large'),
        el('span', { class: 'chatlive__row-name' }, message.from), el('span', { class: 'chatlive__row-text' }, message.text), el('span', { class: 'visually-hidden' }));
      button.addEventListener('click', () => {
        picked = key;
        openRoom(message.room, true);
      });
      return el('li', {}, button);
    }, (node, key) => {
      const button = node.firstChild;
      const unread = unreadItems.has(activityItems.get(key));
      button.classList.toggle('chatlive__row--unread', unread);
      button.setAttribute('aria-current', String(key === current));
      setText(button.lastChild, unread ? ', unread' : '');
    });

    // the debugger
    if (debug.hidden) return;
    time.textContent = clock(chat.now());
    if (part === 'experiments') {
      const version = chat.version();
      for (const button of versionPick.children) {
        button.setAttribute('aria-checked', String(Number(button.dataset.step) === version));
        button.disabled = migrating;
      }
      setText(versionAbout, VERSIONS[version].replace(/^Step \d+: /, '').replace(/^./, (first) => first.toUpperCase()));
      for (const card of scenarioList.children) {
        // a running scenario cannot start again (chat.scenario refuses it): its Run waits for it
        const left = chat.scenarioLeft(card.dataset.key);
        setText(card.querySelector('.chatlive__card-note'), left ? `Running: ${left} ${left === 1 ? 'step' : 'steps'} to go` : '');
        card.querySelector('button').disabled = left !== null;
      }
      for (const row of personList.children) {
        const name = row.dataset.key;
        // a connection is the application's: only a person who has one can lose it
        const returns = chat.reconnects(name), connected = chat.connected(name);
        setText(row.querySelector('.chatlive__lab-note'), returns !== null ? `Reconnects at ${clock(returns)}`
          : name === you ? (connected ? 'you' : 'you, signed out') : connected ? '' : 'Signed out');
        row.classList.toggle('chatlive__lab-person--lost', returns !== null);
        for (const button of row.querySelectorAll('.chatlive__speed')) {
          button.disabled = !connected;
          if (connected) button.removeAttribute('title');
          else button.title = returns !== null ? 'Already lost' : 'Signed out';
        }
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
          : [el('li', { class: 'chatlive__empty' }, kind ? `No ${KINDS[kind]} entries yet.` : 'Nothing yet. Post a message or run a scenario.')]));
      }
    } else if (part === 'actors') {
      setText(memory, `${stats.actors} actors · ${stats.inMemory} in memory · ${stats.stored} in the store`);
      keyed(actorList, actors.map((actor) => actor.name), (name) => {
        const button = el('button', { type: 'button', class: 'chatlive__actor' },
          el('span', { class: 'chatlive__dot', 'aria-hidden': 'true' }), el('code', { title: name }, name), el('span', { class: 'chatlive__where' }));
        button.addEventListener('click', () => inspect(name));
        return el('li', {}, button);
      }, (node, name) => {
        const actor = actors.find((each) => each.name === name);
        const button = node.firstChild;
        button.setAttribute('aria-current', String(name === inspected));
        button.classList.toggle('chatlive__actor--stored', actor.stored);
        setText(button.lastChild, actor.stored ? 'in the store' : 'in memory');
      });
      const chart = charts[chartKey()];
      if (shown?.stored) {
        if (shownStored !== true) chart?.restart(); // nothing lit: the actor's state is in its blob
        shownStored = true;
        setText(stateLine, `${inspected} waits in the store; its next event loads it.`);
      } else if (shown) {
        shownStored = false;
        chart?.show(shown.value);
        const line = [inspected, ' is in ', statePath(shown.value), ` (step ${chat.version()})`];
        if (stateLine.textContent !== line.join('')) stateLine.replaceChildren(el('code', {}, line[0]), line[1], el('code', {}, line[2]), line[3]);
      }
    }
  }

  inspect(`user.${you}`);
}

/**
 * A small menu's keys, as the avatar's: the arrows, Home and End move through `items()`, Escape
 * closes it and puts focus back on its button, Tab closes it and goes on from the button.
 */
function menuKeys(menu, items, close) {
  menu.addEventListener('keydown', (event) => {
    const all = items();
    const at = all.indexOf(document.activeElement);
    const move = { ArrowDown: at + 1, ArrowUp: at - 1, Home: 0, End: all.length - 1 }[event.key];
    if (move !== undefined) {
      event.preventDefault();
      all[(move + all.length) % all.length].focus();
    } else if (event.key === 'Escape') {
      event.preventDefault();
      close(true);
    } else if (event.key === 'Tab') close(true); // Tab goes on from the button, in the sidebar's order
  });
}

function el(tag, attributes = {}, ...children) {
  const node = document.createElement(tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, value);
  node.append(...children);
  return node;
}
