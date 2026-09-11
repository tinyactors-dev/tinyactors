"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const { createSystem, lowlevel } = require("../../..");
const { createChatMachines } = require("../actors");

function setup(t, options = {}) {
  const machines = createChatMachines({
    graceMs: 5000,
    historyLimit: 3,
    maxRooms: 8,
    ...options,
  });
  const system = createSystem({
    ioprocessors: [{ type: "ws" }, { type: "chat-host" }],
  });
  t.after(() => system.close({ force: true }));
  const directory = system.spawn(machines.directory, { input: {} });
  const chat = { system, machines, directory, controls: [], frames: [] };
  provision(chat, "lobby");
  flush(chat);
  return chat;
}
function provision(chat, name) {
  const actor = chat.system.spawn(chat.machines.room, {
    input: { name, directory: chat.directory.id },
  });
  chat.directory.send("ROOM_CREATED", { name, actor: actor.id });
  return actor;
}
function peer(chat, id) {
  const actor = chat.system.spawn(chat.machines.connection, {
    input: {
      sessionId: id,
      user: id,
      connectionId: id,
      directory: chat.directory.id,
    },
  });
  flush(chat);
  return actor;
}
function flush(chat) {
  for (let attempt = 0; attempt < 100; attempt++) {
    const progress = chat.system.run({ workUnits: 10000 });
    let count = 0;
    for (let lease; (lease = chat.system.ioNext()); ) {
      count++;
      if (lease.request.type === "chat-host") chat.controls.push(lease);
      else {
        chat.frames.push({
          target: lease.request.target,
          value: lease.request.body.content.native,
        });
        lease.complete();
      }
    }
    if (!progress.runnable && !count) {
      assert.deepEqual(chat.system.takeErrors(), []);
      return;
    }
  }
  throw new Error("Actor fixture did not quiesce.");
}

test("pending room provisioning cannot undo a later leave or room selection", (t) => {
  const chat = setup(t),
    alice = peer(chat, "Alice");
  alice.send("COMMAND", { type: "join", room: "lobby" });
  flush(chat);
  alice.send("COMMAND", { type: "create", room: "later" });
  flush(chat);
  assert.equal(chat.controls.length, 1);
  alice.send("COMMAND", { type: "leave" });
  flush(chat);
  const room = provision(chat, "later");
  chat.controls.shift().complete();
  flush(chat);
  assert.equal(alice.snapshot().state.room, null);
  assert.equal(room.snapshot().state.members.size, 0);

  alice.send("COMMAND", { type: "create", room: "last" });
  flush(chat);
  alice.send("COMMAND", { type: "join", room: "lobby" });
  flush(chat);
  const last = provision(chat, "last");
  chat.controls.shift().complete();
  flush(chat);
  assert.equal(alice.snapshot().state.room, "lobby");
  assert.equal(last.snapshot().state.members.size, 0);
});

test("reserved names deduplicate creates and prototype-like names are ordinary rooms", (t) => {
  const chat = setup(t),
    alice = peer(chat, "Alice"),
    bob = peer(chat, "Bob");
  alice.send("COMMAND", { type: "create", room: "__proto__" });
  bob.send("COMMAND", { type: "create", room: "__proto__" });
  flush(chat);
  assert.equal(chat.controls.length, 1);
  const room = provision(chat, "__proto__");
  chat.controls.shift().complete();
  flush(chat);
  assert.equal(room.snapshot().state.members.size, 2);
  assert.equal(alice.snapshot().state.room, "__proto__");
  assert.equal(bob.snapshot().state.room, "__proto__");
  assert.ok(chat.directory.snapshot().state.rooms instanceof Map);

  alice.send("COMMAND", { type: "create", room: "constructor" });
  flush(chat);
  const next = provision(chat, "constructor");
  chat.controls.shift().complete();
  flush(chat);
  assert.equal(next.snapshot().state.members.size, 1);
  assert.equal(room.snapshot().state.members.size, 1);
});

test("offline peers coalesce room and directory snapshots without transport sends", (t) => {
  const chat = setup(t),
    alice = peer(chat, "Alice"),
    bob = peer(chat, "Bob");
  alice.send("COMMAND", { type: "join", room: "lobby" });
  flush(chat);
  alice.send("DETACH", { connectionId: "Alice" });
  flush(chat);
  chat.frames.length = 0;
  bob.send("COMMAND", { type: "create", room: "new_room" });
  flush(chat);
  provision(chat, "new_room");
  chat.controls.shift().complete();
  flush(chat);
  bob.send("COMMAND", { type: "join", room: "lobby" });
  bob.send("COMMAND", { type: "say", text: "missed update" });
  flush(chat);
  assert.equal(
    chat.frames.some((frame) => frame.target === "ws:Alice"),
    false,
  );
  assert.ok(
    alice.snapshot().state.rooms.some((room) => room.name === "new_room"),
  );
  alice.send("ATTACH", { connectionId: "Alice-new" });
  flush(chat);
  assert.ok(
    chat.frames.some(
      (frame) =>
        frame.target === "ws:Alice-new" &&
        frame.value.messages?.some(
          (message) => message.text === "missed update",
        ),
    ),
  );
  assert.equal(chat.system.nextDeadline(), lowlevel.TA_TIME_NEVER);
});

test("old room views and old transport failures cannot replace the current selection", (t) => {
  const chat = setup(t),
    alice = peer(chat, "Alice");
  provision(chat, "garden");
  flush(chat);
  alice.send("COMMAND", { type: "join", room: "lobby" });
  flush(chat);
  alice.send("COMMAND", { type: "join", room: "garden" });
  flush(chat);
  chat.frames.length = 0;
  alice.send("ROOM_VIEW", {
    type: "room",
    room: "lobby",
    members: [],
    messages: [{ id: 99, user: "old", text: "obsolete" }],
  });
  flush(chat);
  assert.equal(alice.snapshot().state.room, "garden");
  assert.equal(chat.frames.length, 0);
  alice.send("ATTACH", { connectionId: "Alice-new" });
  flush(chat);
  chat.system.receive(
    { processor: "ws", actor: alice },
    { type: "error.communication", sendId: "Alice" },
  );
  flush(chat);
  assert.ok(alice.snapshot().active.includes("online"));
});

test("cancelled provisioning retains its reservation but not duplicate waiters", (t) => {
  const chat = setup(t, { maxRooms: 2 }),
    alice = peer(chat, "Alice");
  for (let i = 0; i < 10; i++)
    alice.send("COMMAND", { type: "create", room: "pending" });
  flush(chat);
  assert.equal(chat.controls.length, 1);
  assert.equal(chat.directory.snapshot().state.pending.get("pending").size, 1);
  alice.send("COMMAND", { type: "leave" });
  flush(chat);
  assert.equal(chat.directory.snapshot().state.pending.get("pending").size, 0);
  alice.send("COMMAND", { type: "create", room: "over-limit" });
  flush(chat);
  assert.equal(chat.controls.length, 1);
  assert.ok(chat.frames.some((frame) => frame.value.code === "room_limit"));
  alice.send("COMMAND", { type: "join", room: "pending" });
  flush(chat);
  alice.send("DETACH", { connectionId: "Alice" });
  flush(chat);
  const room = provision(chat, "pending");
  chat.controls.shift().complete();
  flush(chat);
  assert.equal(room.snapshot().state.members.get("Alice").online, false);
  alice.send("ATTACH", { connectionId: "new" });
  flush(chat);
  assert.ok(
    chat.frames.some(
      (frame) =>
        frame.target === "ws:new" &&
        frame.value.room === "pending" &&
        frame.value.members[0].online,
    ),
  );
});

test("online views use flat wire frames and leave rejects obsolete room snapshots", (t) => {
  const chat = setup(t),
    alice = peer(chat, "Alice");
  assert.ok(
    chat.frames.some(
      (frame) =>
        frame.value.type === "rooms" && frame.value.rooms[0].name === "lobby",
    ),
  );
  alice.send("COMMAND", { type: "join", room: "lobby" });
  flush(chat);
  assert.ok(
    chat.frames.some(
      (frame) => frame.value.type === "room" && frame.value.room === "lobby",
    ),
  );
  alice.send("COMMAND", { type: "leave" });
  flush(chat);
  assert.equal(
    chat.frames.filter((frame) => frame.value.type === "room").at(-1).value
      .room,
    null,
  );
  chat.frames.length = 0;
  alice.send("ROOM_VIEW", {
    type: "room",
    room: "lobby",
    members: [],
    messages: [],
  });
  flush(chat);
  assert.equal(alice.snapshot().state.room, null);
  assert.equal(chat.frames.length, 0);
});
