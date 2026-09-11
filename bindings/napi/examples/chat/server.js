"use strict";
const http = require("node:http");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { WebSocketServer, WebSocket } = require("ws");
const { createSystem, lowlevel } = require("../..");
const { createChatMachines } = require("./actors");

function integer(value, fallback, bounds) {
  if (value === undefined) return fallback;
  if (!Number.isSafeInteger(value) || value < bounds[0] || value > bounds[1])
    throw new RangeError(
      "Chat option must be an integer in " + bounds.join(".."),
    );
  return value;
}
function command(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const fields = {
    list: [],
    leave: [],
    create: ["room"],
    join: ["room"],
    say: ["text"],
  };
  if (!Object.hasOwn(fields, value.type)) return null;
  if (
    Object.keys(value).some(
      (key) => key !== "type" && !fields[value.type].includes(key),
    )
  )
    return null;
  if (value.type === "create" || value.type === "join")
    return typeof value.room === "string" &&
      /^[a-z0-9_-]{1,32}$/.test(value.room)
      ? { type: value.type, room: value.room }
      : null;
  if (value.type === "say")
    return typeof value.text === "string" &&
      value.text.trim().length > 0 &&
      value.text.trim().length <= 2000
      ? { type: "say", text: value.text.trim() }
      : null;
  return { type: value.type };
}

// The host owns transport/lifetime resources only. Chat data lives in actors.
async function startChat(options = {}) {
  if (options.onError !== undefined && typeof options.onError !== "function")
    throw new TypeError("onError must be a function.");
  const host = options.host ?? "127.0.0.1";
  const port = integer(options.port, 8080, [0, 65535]);
  const graceMs = integer(options.graceMs, 5000, [1, 600000]);
  const historyLimit = integer(options.historyLimit, 50, [1, 1000]);
  const maxRooms = integer(options.maxRooms, 16, [1, 1000]);
  const maxSessions = integer(options.maxSessions, 128, [1, 10000]);
  const maxBufferedBytes = integer(options.maxBufferedBytes, 1024 * 1024, [
    1024,
    64 * 1024 * 1024,
  ]);
  const heartbeatMs = integer(options.heartbeatMs, 15000, [20, 300000]);
  const handshakeMs = integer(options.handshakeMs, 10000, [20, 60000]);
  const writeTimeoutMs = integer(options.writeTimeoutMs, 5000, [20, 30000]);
  const rateLimit = integer(options.rateLimit, 60, [1, 10000]);
  const maxPayload = 16 * 1024;
  const files = new Map();
  for (const [route, filename, type] of [
    ["/", "index.html", "text/html"],
    ["/index.html", "index.html", "text/html"],
    ["/client.js", "client.js", "text/javascript"],
    ["/style.css", "style.css", "text/css"],
  ])
    files.set(route, {
      body: fs.readFileSync(path.join(__dirname, "public", filename)),
      type,
    });
  const machines = createChatMachines({ graceMs, historyLimit, maxRooms });
  const system = createSystem({
    ioprocessors: [{ type: "ws" }, { type: "chat-host" }],
    limits: {
      actors: maxRooms + maxSessions + 1,
      queuedEvents: 65536,
      ioRequests: 16384,
    },
  });
  const sessions = new Map(),
    tokens = new Map(),
    transports = new Map();
  const peers = new Set(),
    jobs = new Set(),
    retiring = new Set(),
    errors = [];
  let drainHandle = null,
    retireTimer = null,
    heartbeatTimer = null;
  let closing = false,
    closePromise = null,
    started = false;
  const directory = system.spawn(machines.directory, {
    name: "chat:directory",
    input: {},
  });
  const server = http.createServer(serve);
  server.requestTimeout = 10000;
  server.headersTimeout = 10000;
  server.keepAliveTimeout = 1000;
  server.maxConnections = Math.max(32, maxSessions * 4);
  const wss = new WebSocketServer({
    noServer: true,
    maxPayload,
    perMessageDeflate: false,
  });

  function report(error) {
    if (errors.length < 32) errors.push(error);
    if (options.onError) {
      try {
        options.onError(error);
      } catch (observerError) {
        process.emitWarning(observerError);
      }
    } else process.emitWarning(error);
  }
  function fatal(error) {
    report(error);
    void close().catch(report);
  }
  function complete(lease, status) {
    if (lease.completed) return;
    try {
      lease.complete(status);
    } catch (error) {
      if (error.status !== lowlevel.TA_STALE) throw error;
    }
  }
  function terminate(peer) {
    peer.socket.terminate();
  }
  function sendRaw(peer, frame) {
    if (closing || peer.socket.readyState !== WebSocket.OPEN) return false;
    const text = JSON.stringify(frame);
    if (
      peer.socket.bufferedAmount + Buffer.byteLength(text) >
      maxBufferedBytes
    ) {
      terminate(peer);
      return false;
    }
    try {
      peer.socket.send(text, (error) => {
        if (error) terminate(peer);
      });
      return true;
    } catch {
      terminate(peer);
      return false;
    }
  }
  function reject(peer, code, message) {
    sendRaw(peer, { type: "error", code, message });
  }
  function scheduleDrain() {
    if (closing || drainHandle) return;
    drainHandle = setImmediate(() => {
      drainHandle = null;
      try {
        drain();
      } catch (error) {
        fatal(error);
      }
    });
  }
  function finish(job, status) {
    if (job.done) return;
    job.done = true;
    clearTimeout(job.timer);
    jobs.delete(job);
    job.peer.jobs.delete(job);
    try {
      complete(job.lease, status);
    } catch (error) {
      fatal(error);
    }
  }
  function deliver(lease) {
    const request = lease.request;
    const peer = transports.get(request.target);
    if (
      !peer ||
      peer.record?.peer !== peer ||
      peer.socket.readyState !== WebSocket.OPEN
    ) {
      complete(lease, lowlevel.TA_COMMUNICATION_ERROR);
      return;
    }
    let text;
    try {
      text = JSON.stringify(request.body.content.native);
    } catch {
      complete(lease, lowlevel.TA_COMMUNICATION_ERROR);
      terminate(peer);
      return;
    }
    if (
      typeof text !== "string" ||
      peer.socket.bufferedAmount + Buffer.byteLength(text) > maxBufferedBytes
    ) {
      complete(lease, lowlevel.TA_COMMUNICATION_ERROR);
      terminate(peer);
      return;
    }
    const job = { peer, lease, done: false, timer: null };
    jobs.add(job);
    peer.jobs.add(job);
    job.timer = setTimeout(() => {
      finish(job, lowlevel.TA_COMMUNICATION_ERROR);
      terminate(peer);
    }, writeTimeoutMs);
    try {
      peer.socket.send(text, (error) => {
        finish(job, error ? lowlevel.TA_COMMUNICATION_ERROR : lowlevel.TA_OK);
        if (error) terminate(peer);
      });
    } catch {
      finish(job, lowlevel.TA_COMMUNICATION_ERROR);
      terminate(peer);
    }
  }
  function sweepRetired() {
    retireTimer = null;
    if (closing) return;
    for (const record of retiring) {
      try {
        record.actor.release();
        retiring.delete(record);
        sessions.delete(record.sessionId);
        tokens.delete(record.token);
        if (record.peer) terminate(record.peer);
      } catch (error) {
        if (error.status === lowlevel.TA_STALE) {
          retiring.delete(record);
          sessions.delete(record.sessionId);
          tokens.delete(record.token);
        } else if (
          error.status !== lowlevel.TA_BUSY ||
          ++record.retries > 1000
        ) {
          fatal(error);
          return;
        }
      }
    }
    if (retiring.size) retireTimer = setTimeout(sweepRetired, 10);
  }
  function hostEffect(lease) {
    const request = lease.request,
      data = request.body.content.native;
    if (
      request.target === "host:create-room" &&
      request.event.name === "CREATE_ROOM"
    ) {
      let actor;
      try {
        actor = system.spawn(machines.room, {
          input: { name: data.name, directory: directory.id },
        });
        directory.send("ROOM_CREATED", { name: data.name, actor: actor.id });
      } catch (error) {
        // A failed publication after allocation requires full host cleanup;
        // do not leave a live room that the directory does not own.
        if (actor) throw error;
        directory.send("ROOM_FAILED", { name: data.name });
        complete(lease, lowlevel.TA_COMMUNICATION_ERROR);
        if (![lowlevel.TA_LIMIT, lowlevel.TA_NO_MEMORY].includes(error.status))
          report(error);
        return;
      }
    } else if (
      request.target === "host:retire-session" &&
      request.event.name === "RETIRE_SESSION"
    ) {
      const record = sessions.get(data.sessionId);
      if (record && record.actor.id === data.actor) {
        record.retiring = true;
        record.retries = 0;
        retiring.add(record);
        tokens.delete(record.token);
      }
      // The origin actor remains pinned until this lease is completed.
      complete(lease, lowlevel.TA_OK);
      if (!retireTimer) sweepRetired();
      return;
    } else {
      complete(lease, lowlevel.TA_UNSUPPORTED);
      return;
    }
    complete(lease, lowlevel.TA_OK);
  }
  function drain() {
    if (closing) return;
    for (let count = 0; count < 64; count++) {
      const lease = system.ioNext();
      if (!lease) return;
      try {
        if (lease.request.kind !== lowlevel.TA_IO_SEND)
          complete(lease, lowlevel.TA_UNSUPPORTED);
        else if (lease.request.type === "ws") deliver(lease);
        else if (lease.request.type === "chat-host") hostEffect(lease);
        else complete(lease, lowlevel.TA_UNSUPPORTED);
      } catch (error) {
        complete(lease, lowlevel.TA_COMMUNICATION_ERROR);
        throw error;
      }
    }
    scheduleDrain();
  }
  function detach(peer) {
    transports.delete("ws:" + peer.id);
    for (const job of peer.jobs)
      finish(
        job,
        closing ? lowlevel.TA_CANCELLED : lowlevel.TA_COMMUNICATION_ERROR,
      );
    const record = peer.record;
    if (!record || record.peer !== peer) return;
    record.peer = null;
    if (closing || record.retiring) return;
    try {
      record.actor.send("DETACH", { connectionId: peer.id });
    } catch (error) {
      if (error.status !== lowlevel.TA_STALE) fatal(error);
    }
  }
  function hello(peer, input) {
    if (
      !input ||
      typeof input !== "object" ||
      Array.isArray(input) ||
      input.type !== "hello" ||
      Object.keys(input).some(
        (key) => !["type", "name", "resume"].includes(key),
      ) ||
      typeof input.name !== "string" ||
      !input.name.trim() ||
      input.name.trim().length > 32 ||
      (input.resume !== undefined &&
        (typeof input.resume !== "string" ||
          !/^[A-Za-z0-9_-]{43}$/.test(input.resume)))
    ) {
      reject(
        peer,
        "invalid_hello",
        "First send hello with a display name of 1 to 32 characters.",
      );
      peer.socket.close(1008);
      return;
    }
    let record = input.resume ? tokens.get(input.resume) : null;
    const resumed = Boolean(record);
    if (input.resume) {
      let status;
      if (record) {
        try {
          status = record.actor.snapshot({ active: false }).status;
        } catch (error) {
          if (error.status !== lowlevel.TA_STALE) throw error;
        }
      }
      if (
        !record ||
        record.retiring ||
        !["starting", "ready", "suspended"].includes(status)
      ) {
        reject(
          peer,
          "session_expired",
          "This session has expired. Connect with a fresh session.",
        );
        peer.socket.close(1008);
        return;
      }
    }
    if (!record) {
      if (sessions.size >= maxSessions) {
        reject(
          peer,
          "session_limit",
          "The server has reached its session limit.",
        );
        peer.socket.close(1013);
        return;
      }
      record = {
        sessionId: crypto.randomUUID(),
        token: crypto.randomBytes(32).toString("base64url"),
        user: input.name.trim(),
        actor: null,
        peer: null,
        retiring: false,
        retries: 0,
      };
      record.actor = system.spawn(machines.connection, {
        input: {
          sessionId: record.sessionId,
          user: record.user,
          connectionId: peer.id,
          directory: directory.id,
        },
      });
      sessions.set(record.sessionId, record);
      tokens.set(record.token, record);
    } else {
      record.actor.send("ATTACH", { connectionId: peer.id });
      const old = record.peer;
      if (old) {
        transports.delete("ws:" + old.id);
        old.socket.close(4000, "Replaced");
      }
    }
    record.peer = peer;
    peer.record = record;
    clearTimeout(peer.handshakeTimer);
    peer.handshakeTimer = null;
    transports.set("ws:" + peer.id, peer);
    sendRaw(peer, {
      type: "welcome",
      session: record.token,
      sessionId: record.sessionId,
      user: record.user,
      resumed,
      graceMs,
    });
  }
  function receive(peer, data, binary) {
    if (closing || peer.socket.readyState !== WebSocket.OPEN) return;
    const now = Date.now();
    if (now - peer.window >= 1000) {
      peer.window = now;
      peer.count = 0;
    }
    if (++peer.count > rateLimit) {
      reject(
        peer,
        "rate_limit",
        "Too many frames. Reconnect after a short pause.",
      );
      peer.socket.close(1008);
      return;
    }
    if (binary) {
      reject(peer, "binary_frame", "Chat commands must be JSON text.");
      peer.socket.close(1003);
      return;
    }
    let input;
    try {
      input = JSON.parse(data.toString("utf8"));
    } catch {
      reject(peer, "invalid_json", "Expected a JSON object.");
      return;
    }
    if (!peer.record) {
      hello(peer, input);
      return;
    }
    if (peer.record.peer !== peer) return;
    const parsed = command(input);
    if (!parsed) {
      reject(
        peer,
        "invalid_command",
        "Unknown command or invalid room/message.",
      );
      return;
    }
    peer.record.actor.send("COMMAND", parsed);
  }
  function connected(socket) {
    const peer = {
      socket,
      id: crypto.randomUUID(),
      record: null,
      alive: true,
      window: Date.now(),
      count: 0,
      jobs: new Set(),
      handshakeTimer: null,
    };
    peers.add(peer);
    peer.handshakeTimer = setTimeout(() => terminate(peer), handshakeMs);
    socket.on("pong", () => {
      peer.alive = true;
    });
    socket.on("message", (data, binary) => {
      try {
        receive(peer, data, binary);
      } catch (error) {
        if ([lowlevel.TA_LIMIT, lowlevel.TA_NO_MEMORY].includes(error.status)) {
          reject(peer, "overloaded", "The server cannot accept more work.");
          terminate(peer);
        } else fatal(error);
      }
    });
    socket.on("error", () => {
      terminate(peer);
    });
    socket.once("close", () => {
      clearTimeout(peer.handshakeTimer);
      peers.delete(peer);
      detach(peer);
    });
  }
  function serve(req, res) {
    let asset;
    try {
      asset = files.get(new URL(req.url, "http://localhost").pathname);
    } catch {
      res.writeHead(400);
      res.end("Bad request");
      return;
    }
    if (!asset || !["GET", "HEAD"].includes(req.method)) {
      res.writeHead(404);
      res.end("Not found");
      return;
    }
    res.writeHead(200, {
      "content-type": asset.type + "; charset=utf-8",
      "content-length": asset.body.length,
      "cache-control": "no-store",
      "x-content-type-options": "nosniff",
      "content-security-policy":
        "default-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; object-src 'none'",
      "referrer-policy": "no-referrer",
    });
    res.end(req.method === "HEAD" ? undefined : asset.body);
  }
  function upgrade(req, socket, head) {
    socket.on("error", () => socket.destroy());
    let pathname;
    try {
      pathname = new URL(req.url, "http://localhost").pathname;
    } catch {
      socket.destroy();
      return;
    }
    if (closing || pathname !== "/chat" || peers.size >= maxSessions * 2) {
      socket.end(
        "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n",
      );
      return;
    }
    if (req.headers.origin) {
      let origin;
      try {
        origin = new URL(req.headers.origin);
      } catch {
        socket.destroy();
        return;
      }
      if (
        !["http:", "https:"].includes(origin.protocol) ||
        origin.host !== req.headers.host
      ) {
        socket.end("HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n");
        return;
      }
    }
    wss.handleUpgrade(req, socket, head, connected);
  }
  function heartbeat() {
    for (const peer of peers) {
      if (!peer.alive) terminate(peer);
      else {
        peer.alive = false;
        try {
          peer.socket.ping();
        } catch {
          terminate(peer);
        }
      }
    }
  }
  function close() {
    if (closePromise) return closePromise;
    closing = true;
    closePromise = Promise.resolve().then(async () => {
      system.stop();
      clearInterval(heartbeatTimer);
      clearImmediate(drainHandle);
      clearTimeout(retireTimer);
      for (const peer of peers) {
        clearTimeout(peer.handshakeTimer);
        terminate(peer);
      }
      for (const job of jobs) finish(job, lowlevel.TA_CANCELLED);
      await Promise.all([
        new Promise((resolve) => {
          server.close(() => resolve());
          server.closeAllConnections();
        }),
        new Promise((resolve) => wss.close(() => resolve())),
      ]);
      // Transports are stopped and every taken write lease has been settled.
      system.close({ force: true });
      sessions.clear();
      tokens.clear();
      transports.clear();
      retiring.clear();
      peers.clear();
    });
    return closePromise;
  }

  server.on("upgrade", upgrade);
  server.on("error", (error) => {
    if (started) fatal(error);
  });
  try {
    const lobby = system.spawn(machines.room, {
      input: { name: "lobby", directory: directory.id },
    });
    directory.send("ROOM_CREATED", { name: "lobby", actor: lobby.id });
    while (system.run({ workUnits: 1000 }).runnable) {}
    await new Promise((resolve, reject) => {
      server.once("error", reject);
      server.listen(port, host, () => {
        server.off("error", reject);
        resolve();
      });
    });
    started = true;
    system.start({
      workUnits: 1000,
      actorQuantum: 100,
      onIO: scheduleDrain,
      onError: fatal,
    });
    heartbeatTimer = setInterval(heartbeat, heartbeatMs);
    heartbeatTimer.unref();
    const address = server.address();
    const displayHost = address.address.includes(":")
      ? "[" + address.address + "]"
      : address.address;
    return {
      url: "http://" + displayHost + ":" + address.port,
      system,
      close,
      errors,
    };
  } catch (error) {
    await close();
    throw error;
  }
}
module.exports = { startChat };
