"use strict";
const assert = require("node:assert/strict");
const { defineMachine, createSystem, send, cancel, lowlevel } = require("..");

// One ephemeral logical browser connection. A physical WebSocket can reconnect
// during a grace period. After the deadline, release this actor and spawn a new
// incarnation. Socket objects stay in the host, not in cloneable actor state.
const remember = ({ state, event }) => {
  state.latest = event.data;
};
const sync = send("SYNC", {
  processor: "ws",
  to: ({ state }) => "ws:" + state.connectionId,
  data: ({ state }) => state.latest,
});
const browserConnection = defineMachine({
  id: "browser-connection",
  state: ({ input }) => ({
    connectionId: input.connectionId,
    latest: input.latest,
  }),
  initial: "online",
  states: {
    online: {
      entry: sync,
      on: {
        UPDATE: { actions: [remember, sync] },
        DISCONNECT: "grace",
        "error.communication": "grace",
      },
    },
    grace: {
      entry: send("EXPIRE", { delayMs: 1000, id: "grace-timeout" }),
      exit: cancel("grace-timeout"),
      on: {
        UPDATE: { actions: remember }, // Coalesce updates while offline.
        RECONNECT: {
          target: "online",
          actions: ({ state, event }) => {
            state.connectionId = event.data.connectionId;
          },
        },
        EXPIRE: "closed",
      },
    },
    closed: { final: true },
  },
});

async function main() {
  const connections = new Map();
  const outbound = [];
  // Replace these tiny transport objects with the WebSocket implementation used
  // by your server. Admission, backpressure, and write completion belong here.
  const attach = (id) =>
    connections.set("ws:" + id, {
      async send(json) {
        outbound.push({ connection: id, ...JSON.parse(json) });
      },
    });
  const system = createSystem({
    ioprocessors: [{ type: "ws", aliases: ["websocket"] }],
  });
  const drain = () =>
    system.drainIO(async (request) => {
      assert.equal(request.kind, lowlevel.TA_IO_SEND);
      const socket = connections.get(request.target);
      if (!socket) return lowlevel.TA_COMMUNICATION_ERROR;
      await socket.send(JSON.stringify(request.body.content.native));
    });
  try {
    attach("c1");
    const actor = system.spawn(browserConnection, {
      name: "browser:alice",
      input: { connectionId: "c1", latest: { revision: 0 } },
    });
    system.run({ nowNs: 0n });
    await drain();
    system.send("browser:alice", "UPDATE", { revision: 1 });
    system.run({ nowNs: 1n });
    await drain();

    connections.delete("ws:c1");
    actor.send("DISCONNECT");
    system.run({ nowNs: 100n });
    assert.deepEqual(actor.snapshot().active, ["grace"]);
    actor.send("UPDATE", { revision: 2 });
    system.run({ nowNs: 200n });
    assert.equal(await drain(), 0);

    attach("c2");
    actor.send("RECONNECT", { connectionId: "c2" });
    system.run({ nowNs: 500000000n });
    await drain();
    assert.deepEqual(outbound, [
      { connection: "c1", revision: 0 },
      { connection: "c1", revision: 1 },
      { connection: "c2", revision: 2 },
    ]);
    assert.equal(system.nextDeadline(), lowlevel.TA_TIME_NEVER);
    system.run({ nowNs: 1000000200n }); // The cancelled timeout cannot close c2.
    assert.deepEqual(actor.snapshot().active, ["online"]);

    connections.delete("ws:c2");
    actor.send("DISCONNECT");
    system.run({ nowNs: 1100000000n });
    system.run({ nowNs: 2100000000n });
    assert.equal(actor.snapshot().status, "done");
    actor.release();
    assert.equal(system.stats().actors, 0);
    assert.deepEqual(system.takeErrors(), []);
    system.close();
    console.log(
      "Reconnect, coalescing, timer cancellation, and expiry passed.",
    );
  } finally {
    connections.clear(); // Stop real transports before force cleanup.
    system.close({ force: true });
  }
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
