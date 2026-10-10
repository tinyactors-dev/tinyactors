'use strict';
// A system in a child process, linked to its parent over stdio: length-prefixed
// frames on stdin/stdout (frames.test.js). stdout carries the link, so logs go
// to stderr. Calls the parent's @echo once linked and reports the reply to the
// parent's @inbox; its own @echo answers the parent. Exits when stdin ends.
const { scxml, createActor, createSystem, createLink } = require('../binding');
const { echo } = require('../charts');

const system = createSystem({ systemID: 'kid', logger: (...a) => console.error(...a) });
createActor(echo, { system, systemID: 'echo' }).start();
const link = createLink(system, {
  name: 'parent',
  pingMs: 0,
  transport: () => ({ readable: process.stdin, writable: process.stdout }),
});
const c = scxml.chart();
const kid = c.scxml('kid', { initial: 's', datamodel: 'ecmascript' },
  c.state('s',
    c.transition('CALL', undefined, c.send('PING', { target: 'tinyactors://parent/@echo' }, c.param('tag', "'from-kid'"))),
    c.transition('PONG', undefined, c.send('GOT', { target: 'tinyactors://parent/@inbox' }, c.param('tag', '_event.data.tag')))));
const actor = createActor(kid, { system }).start();
link.opened.then(() => actor.send({ type: 'CALL' }));
process.stdin.on('end', () => process.exit(0));
