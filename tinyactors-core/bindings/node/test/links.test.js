'use strict';
// Links v1 on the native router (docs/protocol.md): sends across systems over
// a WebSocket, policy (default deny), resumption of ephemeral peers, queue TTL
// -> error.communication, relaying, and system.deliver from host code.
const test = require('node:test');
const assert = require('node:assert/strict');
const { createActor, createSystem, createLink, serveLinks } = require('./binding');
const { echo, caller, notifier, until } = require('./charts');

const ctx = (a) => a.getSnapshot().context;

test('links: sends across systems, replies to _event.origin, reconnects by itself', async () => {
  const server = createSystem({ systemID: 'server' });
  createActor(echo, { system: server, systemID: 'echo' }).start();
  let served = serveLinks(server, { port: 0, exports: ['echo'] });
  await until(() => served.port);
  const port = served.port;

  const browser = createSystem({ systemID: 'browser' });
  const link = createLink(browser, { url: `ws://127.0.0.1:${port}/link`, name: 'server', pingMs: 20 });
  const c = createActor(caller, { system: browser }).start();
  c.send({ type: 'CALL', to: link.get('echo'), tag: 'a' }); // queued by the router until the link opens
  await link.opened;
  await until(() => ctx(c).pongs.length === 1);
  assert.deepEqual(ctx(c).pongs, ['a']);
  assert.equal(link.status().state, 'open');
  assert.equal(link.status().id, 'server');
  await until(() => link.status().sent >= 1);
  assert.deepEqual(served.connections(), ['browser']);

  // the server goes away: the link reconnects by itself, queued envelopes are flushed
  await served.close();
  await until(() => link.status().state === 'reconnecting');
  c.send({ type: 'CALL', to: link.get('echo'), tag: 'b' });
  await until(() => link.status().queued === 1);
  served = serveLinks(server, { port, exports: ['echo'] });
  await until(() => ctx(c).pongs.length === 2, 5000);
  assert.equal(link.status().state, 'open');
  assert.equal(link.status().queued, 0);

  // host code addresses remote actors too
  link.get('echo').send({ type: 'PING', tag: 'host' });
  await until(() => served.status('browser').received >= 2);

  link.close();
  browser.stop();
  await served.close();
  server.stop();
});

test('links: untrusted peers reach only what is exported; refusals raise error.communication', async () => {
  const server = createSystem({ systemID: 'srv1' });
  createActor(echo, { system: server, systemID: 'echo' }).start();
  createActor(echo, { system: server, systemID: 'private' }).start();
  const served = serveLinks(server, { port: 0, exports: { echo: 'PING' } });
  await until(() => served.port);
  const client = createSystem({ systemID: 'cli1' });
  const link = createLink(client, { url: `ws://127.0.0.1:${served.port}/link`, name: 'srv1', pingMs: 0 });
  const c = createActor(caller, { system: client }).start();
  c.send({ type: 'CALL', to: link.get('echo'), tag: 'ok' });
  c.send({ type: 'CALL', to: link.get('private'), tag: 'no' });
  c.send({ type: 'CALL', to: link.get('nobody'), tag: 'gone' });
  await until(() => ctx(c).pongs.length === 1 && ctx(c).errors.length === 2);
  assert.deepEqual(ctx(c).pongs, ['ok']);
  assert.equal(served.status('cli1').state, 'open');
  assert.ok(served.status('cli1').errors >= 2, 'refusals are counted on the route');
  link.close();
  client.stop();
  await served.close();
  server.stop();
});

test('links: an ephemeral peer that reconnects with the same id resumes (queued envelopes delivered)', async () => {
  const server = createSystem({ systemID: 'srv2' });
  const note = createActor(notifier, { system: server, systemID: 'notifier' }).start();
  const served = serveLinks(server, { port: 0, exports: ['notifier'], resumeMs: 5000 });
  await until(() => served.port);
  const sockets = [];
  const client = createSystem({ systemID: 'tab' });
  const link = createLink(client, {
    name: 'srv2',
    pingMs: 0,
    transport: () => {
      const ws = new WebSocket(`ws://127.0.0.1:${served.port}/link`);
      sockets.push(ws);
      return ws;
    },
  });
  const c = createActor(caller, { system: client }).start();
  c.send({ type: 'CALL', to: link.get('notifier'), event: 'SUBSCRIBE' });
  await until(() => ctx(note).sub !== null);
  note.send({ type: 'NOTIFY', tag: 'before' });
  await until(() => ctx(c).pongs.length === 1);

  // the tab drops; the server queues for it
  sockets.at(-1).close();
  await until(() => served.connections().length === 0);
  note.send({ type: 'NOTIFY', tag: 'while-away-1' });
  note.send({ type: 'NOTIFY', tag: 'while-away-2' });
  await until(() => served.status('tab').queued === 2);
  assert.equal(served.status('tab').state, 'reconnecting');

  // the link reconnects by itself with the same system id: resumed, in order
  await until(() => ctx(c).pongs.length === 3, 5000);
  assert.deepEqual(ctx(c).pongs, ['before', 'while-away-1', 'while-away-2']);
  assert.equal(served.status('tab').queued, 0);
  assert.equal(sockets.length, 2);
  link.close();
  client.stop();
  await served.close();
  server.stop();
});

test('links: resumption follows the system clock (a simulated clock forgets the peer)', async () => {
  let now = 0;
  const server = createSystem({ systemID: 'srv3', clock: () => now });
  const note = createActor(notifier, { system: server, systemID: 'notifier' }).start();
  const served = serveLinks(server, { port: 0, exports: ['notifier'], resumeMs: 100 });
  await until(() => served.port);
  const sockets = [];
  const client = createSystem({ systemID: 'tab3' });
  const link = createLink(client, {
    name: 'srv3',
    pingMs: 0,
    transport: () => {
      const ws = new WebSocket(`ws://127.0.0.1:${served.port}/link`);
      sockets.push(ws);
      return ws;
    },
  });
  const c = createActor(caller, { system: client }).start();
  c.send({ type: 'CALL', to: link.get('notifier'), event: 'SUBSCRIBE' });
  await until(() => (server.flush(), ctx(note).sub !== null));
  note.send({ type: 'NOTIFY', tag: 'before' });
  server.flush();
  await until(() => ctx(c).pongs.length === 1);

  sockets.at(-1).close();
  await until(() => served.connections().length === 0);
  note.send({ type: 'NOTIFY', tag: 'while-away' });
  server.flush();
  await until(() => served.status('tab3').queued === 1);

  now += 50; // inside the resumption window: kept
  server.flush();
  assert.equal(served.status('tab3').state, 'reconnecting');
  assert.equal(ctx(note).errors, 0);

  now += 100; // past it on the system's clock (no wall time passed): forgotten
  server.flush();
  assert.equal(served.status('tab3'), null);
  assert.equal(ctx(note).errors, 1); // the queued envelope failed

  await until(() => served.connections().length === 1, 5000);
  assert.equal(served.status('tab3').state, 'open');
  assert.equal(served.status('tab3').received, 0);
  link.close();
  client.stop();
  await served.close();
  server.stop();
});

test('links: envelopes queued past the route TTL expire as error.communication', async () => {
  const client = createSystem({ systemID: 'lonely' });
  const link = createLink(client, { url: 'ws://127.0.0.1:9/link', name: 'nowhere', pingMs: 0, queueTTLMs: 80 });
  const c = createActor(caller, { system: client }).start();
  c.send({ type: 'CALL', to: link.get('echo'), tag: 'x' });
  c.send({ type: 'CALL', to: link.get('echo'), tag: 'y' });
  await until(() => link.status().queued === 2);
  await until(() => ctx(c).errors.length === 2, 3000);
  assert.equal(link.status().queued, 0);
  assert.equal(link.status().errors, 2);
  // fail fast: maxQueue 0 refuses at once
  const fast = createLink(client, { url: 'ws://127.0.0.1:9/link', name: 'nowhere2', pingMs: 0, maxQueue: 0 });
  c.send({ type: 'CALL', to: fast.get('echo'), tag: 'z' });
  await until(() => ctx(c).errors.length === 3);
  link.close();
  fast.close();
  client.stop();
});

test('links: relaying browser -> server -> another system, replies come back the same way', async () => {
  const backend = createSystem({ systemID: 'backend' });
  createActor(echo, { system: backend, systemID: 'echo' }).start();
  const bserved = serveLinks(backend, { port: 0, trusted: true });
  await until(() => bserved.port);

  const edge = createSystem({ systemID: 'edge' });
  const up = createLink(edge, { url: `ws://127.0.0.1:${bserved.port}/link`, name: 'backend', pingMs: 0, announce: true });
  const eserved = serveLinks(edge, { port: 0, trusted: true, announce: true });
  await up.opened;
  await until(() => eserved.port);

  const browser = createSystem({ systemID: 'browser' });
  const link = createLink(browser, { url: `ws://127.0.0.1:${eserved.port}/link`, name: 'edge', pingMs: 0 });
  await link.opened;
  const c = createActor(caller, { system: browser }).start();
  await until(() => bserved.connections().length === 1);
  c.send({ type: 'CALL', to: 'tinyactors://backend/@echo', tag: 'relayed' });
  await until(() => ctx(c).pongs.length === 1);
  assert.deepEqual(ctx(c).pongs, ['relayed']);
  assert.equal(ctx(c).errors.length, 0);

  c.send({ type: 'CALL', to: 'tinyactors://backend/@nobody', tag: 'lost' });
  await until(() => ctx(c).errors.length === 1);

  link.close();
  up.close();
  browser.stop();
  await eserved.close();
  edge.stop();
  await bserved.close();
  backend.stop();
});

test('system.deliver: host code hands envelopes to the router', async () => {
  const system = createSystem({ systemID: 'host-code' });
  const e = createActor(echo, { system, systemID: 'echo' }).start();
  assert.equal(system.deliver({ target: 'tinyactors://host-code/@echo', name: 'PING', data: { tag: 'h' } }), 'delivered');
  assert.equal(ctx(e).n, 1);
  assert.equal(system.deliver({ target: 'tinyactors://host-code/@nobody', name: 'PING' }), 'refused');
  assert.equal(system.deliver({ target: 'tinyactors://elsewhere/@x', name: 'PING' }), 'refused'); // no route
  system.stop();
});

test('system ids: validated, the prefix of every address, what peers know us by', async () => {
  for (const bad of ['Orders', 'a_b', '-a', 'a..b']) assert.throws(() => createSystem({ systemID: bad }), TypeError, bad);
  const server = createSystem({ systemID: 'srv-ids' });
  createActor(echo, { system: server, systemID: 'echo' }).start();
  const served = serveLinks(server, { port: 0, exports: { echo: 'PING' } });
  await until(() => served.port);
  const client = createSystem({ systemID: 'orders-eu.example' });
  const c = createActor(caller, { system: client }).start();
  assert.match(c.address, /^tinyactors:\/\/orders-eu\.example\/#_scxml_[1-9][0-9]*$/);
  const link = createLink(client, { url: `ws://127.0.0.1:${served.port}/link`, name: 'srv-ids', pingMs: 0 });
  c.send({ type: 'CALL', to: link.get('echo'), tag: 'id' });
  await until(() => ctx(c).pongs.length === 1);
  assert.equal(served.status('orders-eu.example').state, 'open');
  assert.equal(link.get('echo').address, 'tinyactors://srv-ids/@echo');
  link.close();
  client.stop();
  await served.close();
  server.stop();
});
