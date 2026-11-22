// The live chat's page: the tutorial's finished charts (charts/, step 5's)
// on @tinyactors/wasm in this tab, behind a chat window. You are one of the
// people at a time; what the window shows is that person's screen, built
// only from what the charts sent to them (chat.mjs). Beside it, any actor's
// chart, lit in the states its host reports (components/live-chart.mjs),
// and a simulated clock the reader runs, speeds up or skips.
import { createChat, PEOPLE } from './chat.mjs';
import { liveChart, statePath } from '/assets/components/live-chart.mjs';

const CHARTS = ['workspace.scxml', 'room.scxml', 'user.scxml'];
const SPEEDS = [1, 10, 60];
const TICK = 250; // wall milliseconds between clock steps while it runs
const AWAY = 300_000; // the user chart's timer
const clock = (seconds) => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, '0')}`;
const STATUS = { active: 'active', away: 'away', offline: 'offline' };

export async function mount(root) {
  root.classList.add('chatlive');
  const status = el('p', { class: 'chatlive__status', 'aria-live': 'polite' }, 'Loading tinyactors…');
  root.append(status);
  const base = new URL('./charts/', import.meta.url);
  const texts = Object.fromEntries(await Promise.all(CHARTS.map(async (name) => [name, await (await fetch(new URL(name, base))).text()])));
  const tinyactors = await import('@tinyactors/wasm');

  let you = 'ada', room = 'general', speed = 1, playing = true, rendering = false, again = false;
  let ready = false; // the page draws once its parts are made
  let inspected = null; // the actor whose chart is shown
  let selfState = null; // your user actor's state value, or null while it is in the store
  let shownStored = null; // whether the inspected actor was in the store when last drawn
  const chat = await createChat(tinyactors, { charts: texts, onChange: () => render() });
  // The people already talking: ada and bob in general, bob's hello.
  await chat.connect('ada');
  await chat.connect('bob');
  await chat.join('ada', 'general');
  await chat.join('bob', 'general');
  await chat.post('bob', 'general', 'hi ada, anyone else around?');

  // ---- the clock
  const play = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary ta-btn--sm' });
  const speeds = el('div', { class: 'ta-tabs ta-tabs--segmented', role: 'radiogroup', 'aria-label': 'Clock speed' });
  for (const value of SPEEDS) {
    const button = el('button', { type: 'button', role: 'radio', class: 'ta-tab', 'aria-checked': String(value === speed) }, `${value}×`);
    button.addEventListener('click', () => {
      speed = value;
      for (const other of speeds.children) other.setAttribute('aria-checked', String(other === button));
    });
    speeds.append(button);
  }
  const skip = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary ta-btn--sm' }, 'Skip 5 minutes');
  const time = el('output', { class: 'chatlive__time', 'aria-label': 'Simulated time' });
  const memory = el('span', { class: 'chatlive__memory' });
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

  // ---- the sidebar: who you are, the rooms, the people
  const youSelect = el('select', { class: 'chatlive__you', 'aria-label': 'You are' },
    ...PEOPLE.map((name) => el('option', { value: name }, name)));
  youSelect.addEventListener('change', () => { you = youSelect.value; inspect(`user.${you}`); render(); });
  const connect = el('button', { type: 'button', class: 'ta-btn ta-btn--secondary ta-btn--sm' });
  connect.addEventListener('click', () => (online(you) ? chat.disconnect(you) : chat.connect(you)));
  const roomList = el('ul', { class: 'chatlive__rooms', 'aria-label': 'Rooms' });
  const newRoom = el('input', { type: 'text', class: 'chatlive__new', placeholder: 'new-room', 'aria-label': 'New room name', maxlength: '40' });
  const create = el('button', { type: 'submit', class: 'ta-btn ta-btn--secondary ta-btn--sm' }, 'Add');
  const createForm = el('form', { class: 'chatlive__create' }, newRoom, create);
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
  const sidebar = el('nav', { class: 'chatlive__sidebar', 'aria-label': 'Workspace' },
    el('label', { class: 'chatlive__label' }, 'You are ', youSelect), connect,
    el('h3', { class: 'chatlive__heading' }, 'Rooms'), roomList, createForm, createNote,
    el('h3', { class: 'chatlive__heading' }, 'People'), people);

  // ---- the room you look at
  const roomTitle = el('h3', { class: 'chatlive__title' });
  const messages = el('ol', { class: 'chatlive__messages', 'aria-live': 'polite', 'aria-label': 'Messages' });
  const text = el('input', { type: 'text', class: 'chatlive__text', 'aria-label': 'Message', maxlength: '200' });
  const send = el('button', { type: 'submit', class: 'ta-btn ta-btn--primary ta-btn--sm' }, 'Send');
  const join = el('button', { type: 'button', class: 'ta-btn ta-btn--primary ta-btn--sm' });
  join.addEventListener('click', () => chat.join(you, room));
  const composer = el('form', { class: 'chatlive__composer' }, text, send);
  let typed = -Infinity; // the simulated time of the last activity sent for typing
  text.addEventListener('input', () => {
    if (!online(you) || chat.now() - typed < 5) return; // one activity per 5 simulated seconds of typing
    typed = chat.now();
    chat.activity(you);
  });
  composer.addEventListener('submit', (event) => {
    event.preventDefault();
    const words = text.value.trim();
    if (!words) return;
    text.value = '';
    chat.post(you, room, words);
  });
  const main = el('section', { class: 'chatlive__room', 'aria-label': 'Room' }, roomTitle, messages, composer, join);

  // ---- inside: any actor's chart, lit in its states
  const actorList = el('ul', { class: 'chatlive__actors', 'aria-label': 'Actors' });
  const frame = el('div', { class: 'chatlive__chart' });
  const stateLine = el('p', { class: 'chatlive__state', 'aria-live': 'polite' });
  const inside = el('aside', { class: 'chatlive__inside', 'aria-label': 'Inside' },
    el('h3', { class: 'chatlive__heading' }, 'Actors'), actorList, frame, stateLine);
  const charts = {};
  function inspect(name) {
    inspected = name;
    const chartName = `${name.split('.')[0]}.scxml`;
    for (const [key, chart] of Object.entries(charts)) chart.view.hidden = key !== chartName;
    charts[chartName] ??= liveChart(texts[chartName], frame);
    charts[chartName].view.hidden = false;
    charts[chartName].restart();
    shownStored = null;
    render();
  }

  root.replaceChildren(
    el('div', { class: 'chatlive__bar' }, el('span', { class: 'chatlive__clock' }, 'Simulated clock'), play, speeds, skip, time, memory),
    el('div', { class: 'chatlive__app' }, sidebar, main, inside),
    status,
  );
  status.textContent = '';
  ready = true;

  // ---- what each part shows, after every change
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
      inspected ? chat.state(inspected) : null]);
    selfState = self.stored ? null : self.value;
    const yourStatus = selfState?.online ?? 'offline';
    time.textContent = clock(chat.now());
    memory.textContent = `${stats.actors} actors · ${stats.inMemory} in memory · ${stats.stored} in the store`;

    // the sidebar
    youSelect.value = you;
    connect.textContent = selfState?.online ? 'Disconnect' : 'Connect';
    const rooms = actors.filter((actor) => actor.chart === 'room').map((actor) => actor.name.slice(5));
    roomList.replaceChildren(...rooms.map((name) => {
      const button = el('button', { type: 'button', class: 'chatlive__room-link', 'aria-current': String(name === room) }, `# ${name}`);
      if (screen.rooms.includes(name)) button.classList.add('chatlive__room-link--joined');
      button.addEventListener('click', () => { room = name; render(); });
      return el('li', {}, button);
    }));
    const roster = { ...screen.roster, [you]: yourStatus };
    people.replaceChildren(...PEOPLE.map((name) => {
      const state = STATUS[roster[name]] ?? 'unknown';
      return el('li', { class: `chatlive__person chatlive__person--${state}` },
        el('span', { class: 'chatlive__dot', 'aria-hidden': 'true' }),
        `${name}${name === you ? ' (you)' : ''}`,
        el('span', { class: 'chatlive__presence' }, state === 'unknown' ? 'not on your screen' : state));
    }));

    // the room
    const joined = screen.rooms.includes(room);
    roomTitle.textContent = `# ${room}`;
    const here = screen.messages.filter((message) => message.room === room);
    messages.replaceChildren(...(here.length ? here.map((message) => el('li', { class: 'chatlive__message' },
      el('span', { class: 'chatlive__from' }, message.from), el('span', {}, message.text)))
      : [el('li', { class: 'chatlive__empty' }, joined ? 'No messages on your screen yet.' : `You have not joined #${room}.`)]),
      ...screen.notices.slice(-1).map((notice) => el('li', { class: 'chatlive__notice' }, notice)));
    messages.scrollTop = messages.scrollHeight;
    const canPost = Boolean(selfState?.online) && joined;
    composer.hidden = !canPost;
    join.hidden = canPost || !selfState?.online;
    join.textContent = `Join #${room}`;
    text.placeholder = `Message #${room} as ${you}`;
    if (!selfState?.online) messages.append(el('li', { class: 'chatlive__notice' }, `${you} is offline: connect to post.`));

    // inside
    actorList.replaceChildren(...actors.map((actor) => {
      const button = el('button', { type: 'button', class: 'chatlive__actor', 'aria-current': String(actor.name === inspected) },
        el('code', {}, actor.name), el('span', { class: 'chatlive__where' }, actor.stored ? 'in the store' : 'in memory'));
      if (actor.stored) button.classList.add('chatlive__actor--stored');
      button.addEventListener('click', () => inspect(actor.name));
      return el('li', {}, button);
    }));
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
