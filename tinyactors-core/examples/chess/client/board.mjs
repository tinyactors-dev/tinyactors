/**
 * <chess-board fen orientation interactive> — renders a position and lets the
 * player pick a move among the legal ones (the server checks again). Fires
 * `move` with { from, to, promotion }. Clones its <template>.
 */
import { fromFEN, legalMoves, square } from '../chess.mjs';

const GLYPH = { k: '♚', q: '♛', r: '♜', b: '♝', n: '♞', p: '♟' };

export class ChessBoard extends HTMLElement {
  static observedAttributes = ['fen', 'orientation', 'interactive', 'last'];
  constructor() {
    super();
    this.attachShadow({ mode: 'open' }).append(document.importNode(document.getElementById('chess-board').content, true));
    this.grid = this.shadowRoot.querySelector('.grid');
    this.selected = null;
    this.grid.addEventListener('click', (e) => this.click(e));
  }
  attributeChangedCallback() {
    this.selected = null;
    this.render();
  }
  get position() {
    const fen = this.getAttribute('fen');
    return fen ? fromFEN(fen) : null;
  }
  click(e) {
    const cell = e.target.closest('[data-sq]');
    if (!cell || !this.hasAttribute('interactive')) return;
    const sq = cell.dataset.sq;
    const pos = this.position;
    const moves = legalMoves(pos);
    if (this.selected && moves.some((m) => m.from === this.selected && m.to === sq)) {
      const promotion = moves.some((m) => m.from === this.selected && m.to === sq && m.promotion) ? 'q' : undefined;
      this.dispatchEvent(new CustomEvent('move', { detail: { from: this.selected, to: sq, promotion }, bubbles: true, composed: true }));
      this.selected = null;
    } else {
      this.selected = moves.some((m) => m.from === sq) ? sq : null;
    }
    this.render();
  }
  render() {
    const pos = this.position;
    if (!pos) return;
    const flip = this.getAttribute('orientation') === 'b';
    const last = (this.getAttribute('last') || '').split('-');
    const targets = this.selected ? new Set(legalMoves(pos).filter((m) => m.from === this.selected).map((m) => m.to)) : new Set();
    const cells = [];
    for (let row = 0; row < 8; row++)
      for (let col = 0; col < 8; col++) {
        const r = flip ? row : 7 - row;
        const f = flip ? 7 - col : col;
        const i = r * 8 + f;
        const sq = square(i);
        const p = pos.board[i];
        const cell = document.createElement('button');
        cell.className = `sq ${(r + f) % 2 ? 'light' : 'dark'}`;
        cell.dataset.sq = sq;
        cell.setAttribute('aria-label', sq);
        if (last.includes(sq)) cell.classList.add('last');
        if (sq === this.selected) cell.classList.add('selected');
        if (targets.has(sq)) cell.classList.add(p === '.' ? 'target' : 'capture');
        if (p !== '.') {
          const piece = document.createElement('span');
          piece.className = `piece ${p === p.toUpperCase() ? 'white' : 'black'}`;
          piece.textContent = GLYPH[p.toLowerCase()];
          cell.append(piece);
        }
        if (col === 0) cell.dataset.rank = r + 1;
        if (row === 7) cell.dataset.file = 'abcdefgh'[f];
        cells.push(cell);
      }
    this.grid.replaceChildren(...cells);
  }
}
customElements.define('chess-board', ChessBoard);
