// The rules engine against known perft counts (castling, en passant, pins,
// promotion) and the game-ending states.
import test from 'node:test';
import assert from 'node:assert/strict';
import { START, fromFEN, toFEN, perft, play, status, legalMoves } from '../chess.mjs';

test('perft: start position', () => {
  const p = fromFEN(START);
  assert.equal(perft(p, 1), 20);
  assert.equal(perft(p, 2), 400);
  assert.equal(perft(p, 3), 8902);
});

test('perft: "Kiwipete" (castling, pins, en passant)', () => {
  const p = fromFEN('r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1');
  assert.equal(perft(p, 1), 48);
  assert.equal(perft(p, 2), 2039);
  assert.equal(perft(p, 3), 97862);
});

test('perft: endgame with en passant discovered checks', () => {
  const p = fromFEN('8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1');
  assert.equal(perft(p, 3), 2812);
  assert.equal(perft(p, 4), 43238);
});

test('perft: promotions', () => {
  const p = fromFEN('n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1');
  assert.equal(perft(p, 3), 9483);
});

test('fool\'s mate, stalemate, round trip', () => {
  let p = fromFEN(START);
  for (const [from, to] of [['f2', 'f3'], ['e7', 'e5'], ['g2', 'g4'], ['d8', 'h4']]) p = play(p, { from, to });
  assert.deepEqual(status(p), { state: 'checkmate', winner: 'b' });
  assert.equal(play(fromFEN(START), { from: 'e2', to: 'e5' }), null, 'illegal');
  assert.deepEqual(status(fromFEN('7k/5Q2/6K1/8/8/8/8/8 b - - 0 1')), { state: 'stalemate', reason: 'stalemate' });
  assert.equal(toFEN(fromFEN(START)), START);
  assert.equal(legalMoves(fromFEN('4k3/P7/8/8/8/8/8/4K3 w - - 0 1')).filter((m) => m.promotion).length, 4);
});
