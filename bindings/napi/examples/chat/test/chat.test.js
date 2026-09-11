"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const { once } = require("node:events");
const WebSocket = require("ws");
const { startChat } = require("../server");

class Client {
  constructor(url, options) {
    this.frames = [];
    this.waiters = new Set();
    this.socket = new WebSocket(url.replace(/^http/, "ws") + "/chat", options);
    this.socket.on("error", () => {});
    this.closed = new Promise(resolve => this.socket.once("close", resolve));
    this.socket.on("message", data => {
      const frame = JSON.parse(data.toString());
      this.frames.push(frame);
      for (const wake of this.waiters) wake();
    });
  }
  async open(hello) {
    await once(this.socket, "open");
    this.send(hello);
    return this;
  }
  send(frame) { this.socket.send(JSON.stringify(frame)); }
  waitFor(predicate, options = {}) {
    const from = options.from ?? 0;
    return new Promise((resolve, reject) => {
      const finish = () => {
        const frame = this.frames.slice(from).find(predicate);
        if (!frame) return;
        clearTimeout(timer);this.waiters.delete(finish);resolve(frame);
      };
      const timer = setTimeout(() => {
        this.waiters.delete(finish);
        reject(new Error("Timed out; frame types: " + this.frames.map(frame => frame.type + (frame.room ? ":" + frame.room : "")).join(", ")));
      }, options.timeout ?? 3000);
      this.waiters.add(finish);finish();
    });
  }
  terminate() { this.socket.terminate(); }
}
async function setup(t, options = {}) {
  const chat = await startChat({ port: 0, ...options });
  t.after(() => chat.close());
  return chat;
}
async function connect(t, chat, hello) {
  const client = new Client(chat.url);
  t.after(() => client.terminate());
  await client.open({ type: "hello", ...hello });
  client.welcome = await client.waitFor(frame => frame.type === "welcome");
  return client;
}
function actors(chat) {
  const result = [];let page;
  do {
    page = chat.system.inspect(record => {
      if (record.kind === "actor") result.push({ id: record.actor, ...record.info });
    }, { cursor: page?.cursor, workUnits: 1 });
  } while (!page.done);
  return result;
}
function room(name, predicate = () => true) {
  return frame => frame.type === "room" && frame.room === name && predicate(frame);
}
async function eventually(check, timeout = 3000) {
  const end = Date.now() + timeout;
  while (!check()) {
    if (Date.now() > end) throw new Error("Condition did not become true.");
    await new Promise(resolve => setTimeout(resolve, 10));
  }
}

test("real clients chat in actor-owned rooms with isolation, history bounds and leave", { timeout: 10000 }, async t => {
  const chat = await setup(t, { historyLimit: 3 });
  const alice = await connect(t, chat, { name: "Alice" });
  const bob = await connect(t, chat, { name: "Bob" });
  const carol = await connect(t, chat, { name: "Carol" });
  alice.send({ type: "create", room: "general" });
  await alice.waitFor(room("general"));
  bob.send({ type: "join", room: "general" });
  await bob.waitFor(room("general", frame => frame.members.length === 2));
  carol.send({ type: "create", room: "private" });
  await carol.waitFor(room("private"));
  const carolMark = carol.frames.length;
  alice.send({ type: "say", text: "hello Bob" });
  const delivered = await bob.waitFor(room("general", frame => frame.messages.some(message => message.text === "hello Bob")));
  assert.equal(delivered.messages.at(-1).user, "Alice");
  for (const text of ["one", "two", "three", "four", "five"]) alice.send({ type: "say", text });
  const bounded = await bob.waitFor(room("general", frame => frame.messages.at(-1)?.text === "five"));
  assert.deepEqual(bounded.messages.map(message => message.text), ["three", "four", "five"]);
  assert.equal(new Set(bounded.messages.map(message => message.id)).size, 3);
  assert.ok(bounded.messages[0].id < bounded.messages[2].id);
  assert.equal(carol.frames.slice(carolMark).some(frame => frame.type === "room" && frame.room === "general"), false);

  const native = actors(chat);
  assert.equal(native.filter(actor => actor.state.kind === "directory").length, 1);
  assert.equal(native.filter(actor => actor.state.kind === "room").length, 3);
  assert.equal(native.filter(actor => actor.state.kind === "connection").length, 3);
  assert.equal(chat.system.stats().actors, 7);

  const bobLeave = bob.frames.length, aliceLeave = alice.frames.length;
  bob.send({ type: "leave" });
  await bob.waitFor(room(null), { from: bobLeave });
  await alice.waitFor(room("general", frame => frame.members.length === 1), { from: aliceLeave });
  const mark = bob.frames.length;
  bob.send({ type: "say", text: "must not post" });
  await bob.waitFor(frame => frame.type === "error", { from: mark });
  alice.send({ type: "say", text: "after leave" });
  await alice.waitFor(room("general", frame => frame.messages.at(-1)?.text === "after leave"));
  assert.equal(bob.frames.slice(mark).some(frame => frame.messages?.some(message => message.text === "after leave")), false);
  assert.deepEqual(chat.system.takeErrors(), []);
});

test("concurrent room creation reserves a single room actor and enforces the room limit", { timeout: 10000 }, async t => {
  const chat = await setup(t, { maxRooms: 2 });
  const alice = await connect(t, chat, { name: "Alice" });
  const bob = await connect(t, chat, { name: "Bob" });
  alice.send({ type: "create", room: "together" });
  bob.send({ type: "create", room: "together" });
  await Promise.all([alice.waitFor(room("together", frame => frame.members.length === 2)), bob.waitFor(room("together", frame => frame.members.length === 2))]);
  assert.equal(actors(chat).filter(actor => actor.state.kind === "room").length, 2);
  const mark = alice.frames.length;
  alice.send({ type: "create", room: "overflow" });
  await alice.waitFor(frame => frame.type === "error", { from: mark });
  assert.equal(actors(chat).filter(actor => actor.state.kind === "room").length, 2);
  assert.deepEqual(chat.system.takeErrors(), []);
});

test("disconnect grace resumes the same actor, coalesces history, cancels expiry and later retires", { timeout: 10000 }, async t => {
  const graceMs = 160;
  const chat = await setup(t, { graceMs });
  const alice = await connect(t, chat, { name: "Alice" });
  const bob = await connect(t, chat, { name: "Bob" });
  alice.send({ type: "join", room: "lobby" });bob.send({ type: "join", room: "lobby" });
  await bob.waitFor(room("lobby", frame => frame.members.length === 2));
  const incarnation = actors(chat).find(actor => actor.state.sessionId === alice.welcome.sessionId).id;
  alice.terminate();await alice.closed;
  await bob.waitFor(room("lobby", frame => frame.members.some(member => member.name === "Alice" && !member.online)));
  bob.send({ type: "say", text: "while offline" });
  await bob.waitFor(room("lobby", frame => frame.messages.at(-1)?.text === "while offline"));
  const resumed = await connect(t, chat, { name: "Not Alice", resume: alice.welcome.session });
  assert.equal(resumed.welcome.resumed, true);assert.equal(resumed.welcome.user, "Alice");
  assert.equal(resumed.welcome.sessionId, alice.welcome.sessionId);
  await resumed.waitFor(room("lobby", frame => frame.messages.some(message => message.text === "while offline")));
  assert.equal(actors(chat).find(actor => actor.state.sessionId === alice.welcome.sessionId).id, incarnation);
  await new Promise(resolve => setTimeout(resolve, graceMs + 30));
  assert.ok(chat.system.actor(incarnation).snapshot().active.includes("online"));

  const takeover = await connect(t, chat, { name: "Alice", resume: resumed.welcome.session });
  await resumed.closed;
  await takeover.waitFor(room("lobby"));
  assert.ok(chat.system.actor(incarnation).snapshot().active.includes("online"));
  const mark = bob.frames.length;
  takeover.terminate();await takeover.closed;
  await bob.waitFor(room("lobby", frame => frame.members.every(member => member.name !== "Alice")), { from: mark });
  await eventually(() => actors(chat).filter(actor => actor.state.kind === "connection").length === 1);
  assert.throws(() => chat.system.actor(incarnation).snapshot());
  const expired = new Client(chat.url);t.after(() => expired.terminate());
  await expired.open({ type: "hello", name: "Alice", resume: takeover.welcome.session });
  await expired.waitFor(frame => frame.type === "error" && frame.code === "session_expired");
  assert.deepEqual(chat.system.takeErrors(), []);
});

test("HTTP client assets, command validation and session admission are bounded", { timeout: 10000 }, async t => {
  const chat = await setup(t, { maxSessions: 1 });
  const page = await fetch(chat.url + "/");assert.equal(page.status, 200);assert.match(await page.text(), /chat/i);
  assert.equal((await fetch(chat.url + "/client.js")).status, 200);
  assert.equal((await fetch(chat.url + "/not-a-file")).status, 404);
  const alice = await connect(t, chat, { name: "Alice" });
  for (const input of [{ type: "ROOM_CREATED", name: "spoof" }, { type: "create", room: "../unsafe" }, { type: "say", text: "x".repeat(2001) }]) {
    const from = alice.frames.length;alice.send(input);
    await alice.waitFor(frame => frame.type === "error", { from });
  }
  assert.equal(actors(chat).filter(actor => actor.state.kind === "room").length, 1);
  const rejected = new Client(chat.url);t.after(() => rejected.terminate());
  await rejected.open({ type: "hello", name: "Bob" });
  await rejected.waitFor(frame => frame.type === "error");
  assert.equal(actors(chat).filter(actor => actor.state.kind === "connection").length, 1);
  assert.deepEqual(chat.system.takeErrors(), []);
});

test("cross-origin browser upgrades are refused", { timeout: 5000 }, async t => {
  const chat = await setup(t);
  const socket = new WebSocket(chat.url.replace(/^http/, "ws") + "/chat", { origin: "http://untrusted.example" });
  socket.on("error", () => {});t.after(() => socket.terminate());
  const [, response] = await once(socket, "unexpected-response");
  assert.equal(response.statusCode, 403);response.resume();socket.terminate();
});

test("close settles live sockets and is idempotent", { timeout: 5000 }, async t => {
  const chat = await setup(t);
  const alice = await connect(t, chat, { name: "Alice" });
  alice.send({ type: "join", room: "lobby" });await alice.waitFor(room("lobby"));
  await chat.close();await alice.closed;await chat.close();
  assert.throws(() => chat.system.stats(), /closed|released/);
});

test("silent handshakes and missing heartbeats release sockets and actors", { timeout: 5000 }, async t => {
  const chat = await setup(t, { handshakeMs: 40, heartbeatMs: 30, graceMs: 30 });
  const silent = new Client(chat.url);t.after(() => silent.terminate());
  await once(silent.socket, "open");await silent.closed;
  assert.equal(actors(chat).filter(actor => actor.state.kind === "connection").length, 0);
  const dead = new Client(chat.url, { autoPong: false });t.after(() => dead.terminate());
  await dead.open({ type: "hello", name: "No pong" });
  await dead.waitFor(frame => frame.type === "welcome");await dead.closed;
  await eventually(() => actors(chat).filter(actor => actor.state.kind === "connection").length === 0);
  assert.deepEqual(chat.errors, []);
});

test("binary, oversized and excessive inbound frames are refused", { timeout: 5000 }, async t => {
  const chat = await setup(t, { rateLimit: 3 });
  const binary = await connect(t, chat, { name: "Binary" });
  binary.socket.send(Buffer.from("not JSON"));
  await binary.waitFor(frame => frame.code === "binary_frame");
  assert.equal(await binary.closed, 1003);
  const large = await connect(t, chat, { name: "Large" });
  large.socket.send("x".repeat(16385));
  assert.equal(await large.closed, 1009);
  const rapid = await connect(t, chat, { name: "Rapid" });
  for (let i = 0; i < 3; i++) rapid.send({ type: "list" });
  await rapid.waitFor(frame => frame.code === "rate_limit");
  assert.equal(await rapid.closed, 1008);
  assert.deepEqual(chat.errors, []);
});

test("bounded outgoing snapshots may exceed the incoming frame limit", { timeout: 5000 }, async t => {
  const chat = await setup(t, { historyLimit: 12 });
  const alice = await connect(t, chat, { name: "Alice" });
  alice.send({ type: "join", room: "lobby" });await alice.waitFor(room("lobby"));
  for (let i = 0; i < 10; i++) alice.send({ type: "say", text: String(i) + "x".repeat(1999) });
  const snapshot = await alice.waitFor(room("lobby", frame => frame.messages.length === 10));
  assert.ok(Buffer.byteLength(JSON.stringify(snapshot)) > 16384);
  assert.equal(alice.socket.readyState, WebSocket.OPEN);
  assert.deepEqual(chat.errors, []);
  assert.deepEqual(chat.system.takeErrors(), []);
});

test("outbound pressure detaches and retires only the affected session", { timeout: 5000 }, async t => {
  const chat = await setup(t, { maxBufferedBytes: 1024, graceMs: 40 });
  const alice = await connect(t, chat, { name: "Alice" });
  const bob = await connect(t, chat, { name: "Bob" });
  alice.send({ type: "join", room: "lobby" });await alice.waitFor(room("lobby"));
  bob.send({ type: "create", room: "other" });await bob.waitFor(room("other"));
  alice.send({ type: "say", text: "x".repeat(1500) });
  await alice.closed;
  await eventually(() => actors(chat).filter(actor => actor.state.kind === "connection").length === 1);
  bob.send({ type: "say", text: "still connected" });
  await bob.waitFor(room("other", frame => frame.messages.at(-1)?.text === "still connected"));
  assert.deepEqual(chat.errors, []);
});

test("invalid configuration and occupied ports fail without leaking a server", { timeout: 5000 }, async t => {
  await assert.rejects(startChat({ port: -1 }), RangeError);
  await assert.rejects(startChat({ historyLimit: 0 }), RangeError);
  await assert.rejects(startChat({ onError: true }), TypeError);
  const chat = await setup(t);
  await assert.rejects(startChat({ port: Number(new URL(chat.url).port) }), { code: "EADDRINUSE" });
  const alice = await connect(t, chat, { name: "Alice" });
  assert.equal(alice.welcome.resumed, false);
});
