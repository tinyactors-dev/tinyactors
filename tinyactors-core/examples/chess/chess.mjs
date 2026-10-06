/**
 * Chess rules as pure functions over a small, serializable position (it lives
 * in a game actor's context, survives hibernation and travels over links).
 *
 *   position = { board, turn, castling, ep, halfmove, fullmove }
 *     board     64 characters, a1 = index 0 ... h8 = 63; "PNBRQK" white,
 *               "pnbrqk" black, "." empty
 *     turn      "w" | "b";   castling  e.g. "KQkq" or "";   ep  "e3" or "-"
 *
 * legalMoves(pos) -> [{ from, to, promotion? }], play(pos, move) -> pos | null,
 * status(pos) -> { state: "playing" | "check" | "checkmate" | "stalemate" |
 * "draw", winner?, reason? }. Validated by perft (test/chess.test.mjs).
 */

export const START = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1';

const FILES = 'abcdefgh';
export const square = (i) => FILES[i & 7] + ((i >> 3) + 1);
export const index = (name) => FILES.indexOf(name[0]) + (Number(name[1]) - 1) * 8;
const colorOf = (p) => (p === '.' ? null : p === p.toUpperCase() ? 'w' : 'b');
const enemy = (c) => (c === 'w' ? 'b' : 'w');

export function fromFEN(fen) {
  const [placement, turn, castling, ep, half, full] = fen.trim().split(/\s+/);
  const rows = placement.split('/');
  const board = new Array(64).fill('.');
  rows.forEach((row, r) => {
    let f = 0;
    for (const ch of row) {
      if (/\d/.test(ch)) f += Number(ch);
      else board[(7 - r) * 8 + f++] = ch;
    }
  });
  return { board: board.join(''), turn, castling: castling === '-' ? '' : castling, ep, halfmove: Number(half || 0), fullmove: Number(full || 1) };
}

export function toFEN(pos) {
  const rows = [];
  for (let r = 7; r >= 0; r--) {
    let row = '';
    let empty = 0;
    for (let f = 0; f < 8; f++) {
      const p = pos.board[r * 8 + f];
      if (p === '.') empty++;
      else {
        if (empty) row += empty;
        row += p;
        empty = 0;
      }
    }
    rows.push(row + (empty || ''));
  }
  return `${rows.join('/')} ${pos.turn} ${pos.castling || '-'} ${pos.ep} ${pos.halfmove} ${pos.fullmove}`;
}

const KNIGHT = [[1, 2], [2, 1], [2, -1], [1, -2], [-1, -2], [-2, -1], [-2, 1], [-1, 2]];
const KING = [[1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0], [-1, -1], [0, -1], [1, -1]];
const ROOK = [[1, 0], [-1, 0], [0, 1], [0, -1]];
const BISHOP = [[1, 1], [1, -1], [-1, 1], [-1, -1]];

function at(board, f, r) {
  return f < 0 || f > 7 || r < 0 || r > 7 ? null : board[r * 8 + f];
}

/** Is square `i` attacked by `by`? */
export function attacked(board, i, by) {
  const f = i & 7, r = i >> 3;
  const up = (p) => (by === 'w' ? p.toUpperCase() : p.toLowerCase());
  const pr = by === 'w' ? r - 1 : r + 1; /* attacking pawns stand behind */
  if (at(board, f - 1, pr) === up('p') || at(board, f + 1, pr) === up('p')) return true;
  for (const [df, dr] of KNIGHT) if (at(board, f + df, r + dr) === up('n')) return true;
  for (const [df, dr] of KING) if (at(board, f + df, r + dr) === up('k')) return true;
  for (const [dirs, pieces] of [[ROOK, 'rq'], [BISHOP, 'bq']])
    for (const [df, dr] of dirs)
      for (let k = 1; ; k++) {
        const p = at(board, f + df * k, r + dr * k);
        if (p === null) break;
        if (p === '.') continue;
        if (pieces.includes(p.toLowerCase()) && colorOf(p) === by) return true;
        break;
      }
  return false;
}

export function inCheck(pos, color = pos.turn) {
  const k = pos.board.indexOf(color === 'w' ? 'K' : 'k');
  return k >= 0 && attacked(pos.board, k, enemy(color));
}

function pseudoMoves(pos) {
  const { board, turn } = pos;
  const out = [];
  const add = (from, to, promotion) => out.push(promotion ? { from: square(from), to: square(to), promotion } : { from: square(from), to: square(to) });
  for (let i = 0; i < 64; i++) {
    const p = board[i];
    if (colorOf(p) !== turn) continue;
    const f = i & 7, r = i >> 3, kind = p.toLowerCase();
    if (kind === 'p') {
      const dir = turn === 'w' ? 1 : -1, last = turn === 'w' ? 7 : 0, home = turn === 'w' ? 1 : 6;
      const push = (to) => (((to >> 3) === last) ? ['q', 'r', 'b', 'n'].forEach((q) => add(i, to, q)) : add(i, to));
      if (at(board, f, r + dir) === '.') {
        push(i + 8 * dir);
        if (r === home && at(board, f, r + 2 * dir) === '.') add(i, i + 16 * dir);
      }
      for (const df of [-1, 1]) {
        const t = at(board, f + df, r + dir);
        if (t === null) continue;
        const to = i + 8 * dir + df;
        if (colorOf(t) === enemy(turn) || (pos.ep !== '-' && index(pos.ep) === to)) push(to);
      }
    } else if (kind === 'n' || kind === 'k') {
      for (const [df, dr] of kind === 'n' ? KNIGHT : KING) {
        const t = at(board, f + df, r + dr);
        if (t !== null && colorOf(t) !== turn) add(i, i + df + 8 * dr);
      }
      if (kind === 'k') {
        const rank = turn === 'w' ? 0 : 7, them = enemy(turn);
        const [K, Q] = turn === 'w' ? ['K', 'Q'] : ['k', 'q'];
        if (i === rank * 8 + 4 && !attacked(board, i, them)) {
          if (pos.castling.includes(K) && board[i + 1] === '.' && board[i + 2] === '.' && !attacked(board, i + 1, them) && !attacked(board, i + 2, them))
            add(i, i + 2);
          if (pos.castling.includes(Q) && board[i - 1] === '.' && board[i - 2] === '.' && board[i - 3] === '.' && !attacked(board, i - 1, them) && !attacked(board, i - 2, them))
            add(i, i - 2);
        }
      }
    } else {
      const dirs = kind === 'r' ? ROOK : kind === 'b' ? BISHOP : [...ROOK, ...BISHOP];
      for (const [df, dr] of dirs)
        for (let k = 1; ; k++) {
          const t = at(board, f + df * k, r + dr * k);
          if (t === null || colorOf(t) === turn) break;
          add(i, i + df * k + 8 * dr * k);
          if (t !== '.') break;
        }
    }
  }
  return out;
}

/** The position after `move`, assumed pseudo-legal (no legality check). */
function apply(pos, { from, to, promotion }) {
  const b = pos.board.split('');
  const fi = index(from), ti = index(to);
  const p = b[fi], kind = p.toLowerCase();
  const captured = b[ti];
  let ep = '-';
  if (kind === 'p') {
    if (to === pos.ep) b[ti + (pos.turn === 'w' ? -8 : 8)] = '.'; /* en passant */
    if (Math.abs(ti - fi) === 16) ep = square((fi + ti) / 2);
  }
  if (kind === 'k' && Math.abs(ti - fi) === 2) { /* castling: move the rook too */
    const [rf, rt] = ti > fi ? [fi + 3, fi + 1] : [fi - 4, fi - 1];
    b[rt] = b[rf];
    b[rf] = '.';
  }
  b[ti] = promotion ? (pos.turn === 'w' ? promotion.toUpperCase() : promotion.toLowerCase()) : p;
  b[fi] = '.';
  let castling = pos.castling;
  const strip = (chars) => (castling = castling.replace(new RegExp(`[${chars}]`, 'g'), ''));
  if (p === 'K') strip('KQ');
  if (p === 'k') strip('kq');
  for (const sq of [fi, ti]) {
    if (sq === 0) strip('Q');
    if (sq === 7) strip('K');
    if (sq === 56) strip('q');
    if (sq === 63) strip('k');
  }
  return {
    board: b.join(''),
    turn: enemy(pos.turn),
    castling,
    ep,
    halfmove: kind === 'p' || captured !== '.' ? 0 : pos.halfmove + 1,
    fullmove: pos.turn === 'b' ? pos.fullmove + 1 : pos.fullmove,
  };
}

export function legalMoves(pos) {
  return pseudoMoves(pos).filter((m) => !inCheck(apply(pos, m), pos.turn));
}

/** The position after a legal `move`, or null (promotion defaults to a queen). */
export function play(pos, move) {
  const m = legalMoves(pos).find((x) => x.from === move.from && x.to === move.to && (!x.promotion || x.promotion === (move.promotion || 'q')));
  return m ? apply(pos, m) : null;
}

function insufficient(board) {
  const pieces = board.replace(/[.kK]/g, '');
  return pieces === '' || /^[nNbB]$/.test(pieces);
}

export function status(pos) {
  const moves = legalMoves(pos);
  const check = inCheck(pos);
  if (!moves.length) return check ? { state: 'checkmate', winner: enemy(pos.turn) } : { state: 'stalemate', reason: 'stalemate' };
  if (pos.halfmove >= 100) return { state: 'draw', reason: 'fifty-move rule' };
  if (insufficient(pos.board)) return { state: 'draw', reason: 'insufficient material' };
  return { state: check ? 'check' : 'playing' };
}

/** Leaf count of the legal move tree (the standard move-generator test). */
export function perft(pos, depth) {
  if (depth === 0) return 1;
  let n = 0;
  for (const m of legalMoves(pos)) n += depth === 1 ? 1 : perft(apply(pos, m), depth - 1);
  return n;
}
