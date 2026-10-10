'use strict';
// Charts the binding's tests share, written with the chart helpers, and a
// helper that waits for a condition.
const { scxml } = require('./binding');

const c = scxml.chart();

/* answers PING with PONG { n, tag } to the sender (_event.origin) */
const echo = c.scxml('echo', { initial: 's', datamodel: 'ecmascript' },
  c.datamodel({ n: 0 }),
  c.state('s',
    c.transition('PING', undefined,
      c.assign('n', 'n + 1'),
      c.send('PONG', { targetexpr: '_event.origin' }, c.param('n', 'n'), c.param('tag', '_event.data.tag')))));

/* CALL { to, tag, event?, pad? } sends `event` (PING) to `to`; counts PONGs and error.communication */
const caller = c.scxml('caller', { initial: 's', datamodel: 'ecmascript' },
  c.datamodel({ pongs: [], errors: [] }),
  c.state('s',
    c.transition('CALL', undefined,
      c.send(undefined, { eventexpr: "_event.data.event || 'PING'", targetexpr: 'String(_event.data.to)' }, c.param('tag', '_event.data.tag'), c.param('pad', '_event.data.pad'))),
    c.transition('PONG', undefined, c.script('pongs.push(_event.data.tag)')),
    c.transition('error.communication', undefined, c.script("errors.push(_event.sendid || 'error')"))));

/* SUBSCRIBE remembers its sender; NOTIFY { tag } sends it PONG */
const notifier = c.scxml('notifier', { initial: 's', datamodel: 'ecmascript' },
  c.datamodel({ sub: null, errors: 0 }),
  c.state('s',
    c.transition('SUBSCRIBE', undefined, c.assign('sub', '_event.origin')),
    c.transition('NOTIFY', undefined, c.send('PONG', { targetexpr: 'sub' }, c.param('tag', '_event.data.tag'))),
    c.transition('error.communication', undefined, c.assign('errors', 'errors + 1'))));

const until = async (cond, ms = 4000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error('timed out');
    await new Promise((r) => setTimeout(r, 5));
  }
};

module.exports = { echo, caller, notifier, until };
