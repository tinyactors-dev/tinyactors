/**
 * The chess client: a tinyactors system in the browser (web binding), linked
 * to the server's system. The player is an actor (player.mjs); the lobby and
 * the game are actors on the server, reached through the link as if local.
 * The UI renders snapshots: the player's for the game, the link's for the
 * connection pill.
 */
import { createSystem, createActor, createLink, createMachine, assign, sendTo } from '../../../bindings/web/index.mjs';
import { playerMachine } from './player.mjs';
import './board.mjs';

const template = (id) => document.importNode(document.getElementById(id).content, true);

/** <link-status> — the link actor's snapshot: connecting / open (rtt) / reconnecting / closed */
class LinkStatus extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' }).append(template('link-status'));
  }
  watch(link) {
    const label = this.shadowRoot.querySelector('.label');
    const render = (s) => {
      const c = s.context;
      this.dataset.state = s.value;
      label.textContent =
        s.value === 'open' ? `connected${c.rtt !== null ? ` · ${Math.round(c.rtt)} ms` : ''}`
        : s.value === 'reconnecting' ? `reconnecting (attempt ${c.attempts})${c.queued ? ` · ${c.queued} queued` : ''}`
        : s.value;
    };
    render(link.getSnapshot());
    link.subscribe(render);
  }
}
customElements.define('link-status', LinkStatus);

/** <chess-app> — screens follow the player actor's state */
class ChessApp extends HTMLElement {
  connectedCallback() {
    this.append(template('chess-app'));
    const $ = (sel) => this.querySelector(sel);
    /* a stable system id per tab: after a reload our actors have the same
     * addresses, so the game still reaches us and we can rejoin it */
    let id = sessionStorage.getItem('chess.system');
    if (!id) sessionStorage.setItem('chess.system', (id = crypto.randomUUID().replace(/-/g, '').slice(0, 12)));
    const system = createSystem({ id });
    const url = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/link`;
    const link = createLink(system, { url, name: 'chess' });
    $('link-status').watch(link);

    const name = localStorage.getItem('chess.name') || `Player ${Math.floor(Math.random() * 900 + 100)}`;
    const player = createActor(playerMachine({ createMachine, assign, sendTo }), { system, input: { lobby: link.get('lobby'), name } }).start();
    window.chess = { system, link, player }; /* for the console */

    $('#name').value = name;
    const nameNow = () => {
      const n = $('#name').value.trim() || name;
      localStorage.setItem('chess.name', n);
      return n;
    };
    $('#create').addEventListener('click', () => player.send({ type: 'CREATE', name: nameNow() }));
    $('#join-form').addEventListener('submit', (e) => {
      e.preventDefault();
      player.send({ type: 'JOIN', code: $('#code').value, name: nameNow() });
    });
    $('#resign').addEventListener('click', () => confirm('Resign this game?') && player.send({ type: 'RESIGN' }));
    for (const b of this.querySelectorAll('.new-game')) b.addEventListener('click', () => player.send({ type: 'NEW_GAME' }));
    $('#copy').addEventListener('click', async () => {
      await navigator.clipboard?.writeText($('#share').value).catch(() => {});
      $('#copy').textContent = 'Copied';
      setTimeout(() => ($('#copy').textContent = 'Copy link'), 1500);
    });
    this.addEventListener('move', (e) => player.send({ type: 'MOVE', ...e.detail }));

    /* join from a shared link (?code=...), or rejoin our game after a reload */
    const code = new URLSearchParams(location.search).get('code') || sessionStorage.getItem('chess.code');
    if (code) {
      $('#code').value = code;
      const join = (s) => {
        if (s.value !== 'open' || this.autojoined || player.getSnapshot().value !== 'lobby') return;
        this.autojoined = true;
        player.send({ type: 'JOIN', code, name: nameNow() });
      };
      join(link.getSnapshot());
      link.subscribe(join);
    }
    player.subscribe((s) => {
      if (['waiting', 'playing', 'watching'].includes(s.value) && s.context.code) sessionStorage.setItem('chess.code', s.context.code);
      if (s.value === 'lobby' || s.value === 'over') sessionStorage.removeItem('chess.code');
    });

    const render = (s) => {
      const c = s.context;
      const screen = { lobby: 'lobby', creating: 'lobby', joining: 'lobby', waiting: 'waiting', playing: 'game', watching: 'game', over: 'game' }[s.value];
      for (const el of this.querySelectorAll('[data-screen]')) el.hidden = el.dataset.screen !== screen;
      $('#busy').hidden = !['creating', 'joining'].includes(s.value);
      $('#error').textContent = c.error || '';
      $('#error').hidden = !c.error;
      if (s.value === 'waiting') {
        $('#the-code').textContent = c.code;
        $('#share').value = `${location.origin}${location.pathname}?code=${c.code}`;
      }
      if (screen === 'game' && c.fen) {
        const board = $('chess-board');
        board.setAttribute('fen', c.fen);
        board.setAttribute('orientation', c.color || 'w');
        board.setAttribute('last', c.lastMove ? `${c.lastMove.from}-${c.lastMove.to}` : '');
        const turn = c.fen.split(' ')[1];
        const mine = s.value === 'playing' && c.color === turn && ['playing', 'check'].includes(c.status.state);
        board.toggleAttribute('interactive', mine);
        $('#black-row').style.order = c.color === 'b' ? 2 : 1; /* your side at the bottom, like the board */
        $('#white-row').style.order = c.color === 'b' ? 1 : 2;
        $('#white').textContent = c.names?.w || '—';
        $('#black').textContent = c.names?.b || '—';
        $('#white-row').classList.toggle('turn', turn === 'w' && !['checkmate', 'stalemate', 'draw', 'resigned', 'abandoned'].includes(c.status.state));
        $('#black-row').classList.toggle('turn', turn === 'b' && !['checkmate', 'stalemate', 'draw', 'resigned', 'abandoned'].includes(c.status.state));
        $('#game-code').textContent = c.code;
        $('#role').textContent = c.color ? `You play ${c.color === 'w' ? 'white' : 'black'}` : 'Spectating';
        $('#info').textContent = describe(c, mine);
        $('#moves').textContent = `${c.moves} moves · ${c.spectators} watching`;
        const r = c.record; /* from the records actor in PostgreSQL, if the server has one */
        $('#record').hidden = !r;
        if (r) $('#record').textContent = `Your record: ${r.wins} won · ${r.losses} lost · ${r.draws} drawn (${r.games} games)`;
        $('#resign').hidden = s.value !== 'playing';
        for (const b of this.querySelectorAll('.new-game')) b.hidden = !['over', 'watching'].includes(s.value);
      }
    };
    render(player.getSnapshot());
    player.subscribe(render);
  }
}

function describe(c, mine) {
  const name = (color) => (color === 'w' ? c.names?.w || 'White' : c.names?.b || 'Black');
  switch (c.status.state) {
    case 'checkmate': return `Checkmate — ${name(c.status.winner)} wins`;
    case 'resigned': return `${name(c.status.winner === 'w' ? 'b' : 'w')} resigned — ${name(c.status.winner)} wins`;
    case 'stalemate': return 'Stalemate — draw';
    case 'draw': return `Draw (${c.status.reason})`;
    case 'abandoned': return 'Game abandoned';
    case 'check': return mine ? 'Check! Your move' : 'Check';
    default: return mine ? 'Your move' : c.color ? 'Waiting for your opponent…' : '';
  }
}

customElements.define('chess-app', ChessApp);
