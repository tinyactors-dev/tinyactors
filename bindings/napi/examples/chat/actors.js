"use strict";

const { defineMachine, lowlevel } = require("../..");

const frame = (send, state, value) => {
  if (state.connectionId)
    send("connection.show-frame", {
      processor: "ws",
      to: "ws:" + state.connectionId,
      data: value,
      id: state.connectionId,
    });
};
// A terminal peer may disappear between fanout and delivery. That is a normal
// lifecycle race, not a reason to skip the remaining recipients.
const peerFrame = (send, peer, value) => {
  if (typeof peer !== "bigint") return;
  try {
    send(
      value.type === "rooms"
        ? "connection.show-rooms"
        : value.type === "room" || value.type === "receipt"
          ? "connection.show-room"
          : "connection.show-error",
      { to: peer, data: value },
    );
  } catch (error) {
    if (
      ![
        lowlevel.TA_STALE,
        lowlevel.TA_NOT_FOUND,
        lowlevel.TA_CANCELLED,
      ].includes(error.status)
    )
      throw error;
  }
};
const roomList = (state) =>
  Array.from(state.rooms.entries()).map(([name, value]) => ({
    name,
    members: value.count || 0,
  }));
const listFrame = (send, state, peer) =>
  peerFrame(send, peer, { type: "rooms", rooms: roomList(state) });
const emptyRoom = () => ({
  type: "room",
  room: null,
  members: [],
  messages: [],
});
const emitCached = (send, state) => {
  if (state.rooms.length)
    frame(send, state, { type: "rooms", rooms: state.rooms });
  if (state.room !== null)
    frame(send, state, {
      type: "room",
      room: state.room,
      members: state.members,
      messages: state.messages,
    });
};
const sendRoomView = (send, session, room) => {
  if (room) send("room.snapshot", { to: room.actor, data: { peer: session.peer } });
  else peerFrame(send, session.peer, emptyRoom());
};
const notifyCount = (send, state, name) => {
  send("directory.update-room-count", {
    to: state.directory,
    data: { name, count: state.members ? state.members.size : 0 },
  });
};
const broadcastRoom = (send, state, value) => {
  state.members.forEach((member) => {
    if (member.peer) peerFrame(send, member.peer, value);
  });
};
const roomView = (state) => ({
  type: "room",
  room: state.name,
  members: Array.from(state.members.entries()).map(([id, member]) => ({
    id,
    name: member.name,
    online: !!member.online,
  })),
  messages: state.messages.map((message) => ({
    id: message.id,
    room: message.room,
    user: message.user,
    text: message.text,
    receipts: Array.from(message.seenBy.values()),
  })),
});
const receiptFrame = (state, message) => ({
  type: "receipt",
  room: state.name,
  messageId: message.id,
  receipts: Array.from(message.seenBy.values()),
});
const directoryError = (send, session, detail) =>
  peerFrame(send, session.peer, {
    type: "error",
    code: detail.code,
    message: detail.message,
  });
const validRoom = (name) =>
  typeof name === "string" && /^[a-z0-9_-]{1,32}$/u.test(name);
const validText = (text) =>
  typeof text === "string" && text.trim() && text.trim().length <= 2000;

function makeDirectory(maxRooms) {
  // Allocation is a host effect. Keep its reservation even if its last waiting
  // session leaves, because the host may already be creating that room.
  return defineMachine({
    id: "chat-directory",
    state: () => ({
      kind: "directory",
      rooms: new Map(),
      sessions: new Map(),
      pending: new Map(),
      maxRooms,
    }),
    initial: "running",
    states: {
      running: {
        on: {
          "directory.register": ({ state, event, send }) => {
            const input = event.data || {},
              old = state.sessions.get(input.sessionId);
            if (old) {
              old.user = input.user;
              old.peer = input.peer;
              old.online = true;
            } else {
              state.sessions.set(input.sessionId, {
                sessionId: input.sessionId,
                user: input.user,
                peer: input.peer,
                room: null,
                pending: null,
                online: true,
                version: 0,
              });
            }
            const session = state.sessions.get(input.sessionId);
            if (session.room && state.rooms.has(session.room))
              send("room.presence", {
                to: state.rooms.get(session.room).actor,
                data: { sessionId: input.sessionId, online: true },
              });
            send("connection.select-room", {
              to: session.peer,
              data: { room: session.room, version: session.version },
            });
            listFrame(send, state, session.peer);
            sendRoomView(
              send,
              session,
              session.room ? state.rooms.get(session.room) : null,
            );
          },
          "directory.disconnect": ({ state, event, send }) => {
            const session = state.sessions.get(event.data?.sessionId);
            if (session) {
              session.online = false;
              if (session.room && state.rooms.has(session.room))
                send("room.presence", {
                  to: state.rooms.get(session.room).actor,
                  data: { sessionId: event.data.sessionId, online: false },
                });
            }
          },
          "directory.forget": ({ state, event, send }) => {
            const session = state.sessions.get(event.data?.sessionId);
            if (session) {
              if (session.room && state.rooms.has(session.room))
                send("room.leave", {
                  to: state.rooms.get(session.room).actor,
                  data: { sessionId: event.data.sessionId },
                });
              cancelPending(state, session);
              state.sessions.delete(event.data.sessionId);
            }
          },
          "directory.confirm-room": ({ state, event, send }) => {
            const input = event.data || {};
            if (!validRoom(input.name) || state.rooms.has(input.name)) return;
            state.rooms.set(input.name, { actor: input.actor, count: 0 });
            const pending = state.pending.get(input.name) || new Map();
            state.pending.delete(input.name);
            pending.forEach((version, id) => {
              const session = state.sessions.get(id);
              if (
                session &&
                session.version === version &&
                session.pending === input.name
              )
                joinRoom({ send, state, session, name: input.name });
            });
            broadcastLists(send, state);
          },
          "directory.reject-room": ({ state, event, send }) => {
            const name = event.data?.name,
              pending = state.pending.get(name) || new Map();
            state.pending.delete(name);
            pending.forEach((version, id) => {
              const session = state.sessions.get(id);
              if (
                session &&
                session.version === version &&
                session.pending === name
              ) {
                session.pending = null;
                directoryError(send, session, {
                  code: "room_unavailable",
                  message: "Room could not be created",
                });
              }
            });
          },
          "directory.update-room-count": ({ state, event, send }) => {
            const input = event.data || {};
            if (state.rooms.has(input.name))
              state.rooms.get(input.name).count = input.count || 0;
            broadcastLists(send, state);
          },
          "directory.command": ({ state, event, send }) =>
            handleCommand(send, state, event.data || {}),
        },
      },
    },
  });
}

function cancelPending(state, session) {
  if (session.pending)
    state.pending.get(session.pending)?.delete(session.sessionId);
  session.pending = null;
  session.version++;
}
function queueJoin(state, session, name) {
  cancelPending(state, session);
  session.pending = name;
  state.pending.get(name).set(session.sessionId, session.version);
}
function broadcastLists(send, state) {
  state.sessions.forEach((session) => {
    listFrame(send, state, session.peer);
  });
}
function handleCommand(send, state, input) {
  const session = state.sessions.get(input.sessionId);
  if (!session || !session.online) return;
  if (input.type === "list") return listFrame(send, state, session.peer);
  if (input.type === "create") {
    if (!validRoom(input.room))
      return directoryError(send, session, {
        code: "invalid_room",
        message: "Invalid room name",
      });
    if (state.rooms.has(input.room))
      return joinRoom({ send, state, session, name: input.room });
    if (state.pending.has(input.room)) {
      queueJoin(state, session, input.room);
      return;
    }
    if (state.rooms.size + state.pending.size >= state.maxRooms)
      return directoryError(send, session, {
        code: "room_limit",
        message: "Room limit reached",
      });
    state.pending.set(input.room, new Map());
    queueJoin(state, session, input.room);
    try {
      return send("host.create-room", {
        processor: "chat-host",
        to: "host:create-room",
        data: { name: input.room },
      });
    } catch (error) {
      state.pending.delete(input.room);
      cancelPending(state, session);
      throw error;
    }
  }
  if (input.type === "leave") {
    cancelPending(state, session);
    const old = state.rooms.get(session.room);
    if (old)
      send("room.leave", { to: old.actor, data: { sessionId: input.sessionId } });
    session.room = null;
    send("connection.select-room", {
      to: session.peer,
      data: { room: null, version: session.version },
    });
    return peerFrame(send, session.peer, emptyRoom());
  }
  if (input.type === "join") {
    if (!state.rooms.has(input.room)) {
      if (state.pending.has(input.room)) {
        queueJoin(state, session, input.room);
        return;
      }
      return directoryError(send, session, {
        code: "room_missing",
        message: "Room does not exist",
      });
    }
    return joinRoom({ send, state, session, name: input.room });
  }
  if (input.type === "say") {
    const room = session.room && state.rooms.get(session.room);
    if (!room)
      return directoryError(send, session, {
        code: "not_in_room",
        message: "Join a room first",
      });
    if (!validText(input.text))
      return directoryError(send, session, {
        code: "invalid_message",
        message: "Invalid message",
      });
    return send("room.say", {
      to: room.actor,
      data: { sessionId: input.sessionId, text: input.text.trim() },
    });
  }
  if (input.type === "message.read") {
    const room = session.room && state.rooms.get(session.room);
    if (room && Number.isSafeInteger(input.messageId))
      return send("room.read", {
        to: room.actor,
        data: { sessionId: input.sessionId, messageId: input.messageId },
      });
    return;
  }
  return directoryError(send, session, {
    code: "invalid_command",
    message: "Unknown command",
  });
}
function joinRoom(args) {
  const { send, state, session, name } = args;
  const room = state.rooms.get(name);
  cancelPending(state, session);
  if (session.room && session.room !== name && state.rooms.has(session.room))
    send("room.leave", {
      to: state.rooms.get(session.room).actor,
      data: { sessionId: session.sessionId },
    });
  session.room = name;
  send("connection.select-room", {
    to: session.peer,
    data: { room: name, version: session.version },
  });
  return send("room.join", {
    to: room.actor,
    data: {
      sessionId: session.sessionId,
      user: session.user,
      peer: session.peer,
      online: session.online,
    },
  });
}

function makeRoom(historyLimit) {
  // Each room is the single writer for membership and its history window.
  return defineMachine({
    id: "chat-room",
    state: ({ input }) => ({
      kind: "room",
      name: input.name,
      directory: input.directory,
      members: new Map(),
      messages: [],
      nextId: 1,
    }),
    initial: "running",
    states: {
      running: {
        on: {
          "room.join": ({ state, event, send }) => {
            const input = event.data || {};
            state.members.set(input.sessionId, {
              name: input.user,
              peer: input.peer,
              online: input.online !== false,
            });
            notifyCount(send, state, state.name);
            broadcastRoom(send, state, roomView(state));
          },
          "room.leave": ({ state, event, send }) => {
            state.members.delete(event.data?.sessionId);
            notifyCount(send, state, state.name);
            broadcastRoom(send, state, roomView(state));
          },
          "room.presence": ({ state, event, send }) => {
            const member = state.members.get(event.data?.sessionId);
            if (member) {
              member.online = !!event.data.online;
              broadcastRoom(send, state, roomView(state));
            }
          },
          "room.say": ({ state, event, send }) => {
            const input = event.data || {},
              member = state.members.get(input.sessionId);
            if (!member || !validText(input.text)) return;
            state.messages.push({
              id: state.nextId++,
              room: state.name,
              user: member.name,
              text: input.text.trim(),
              seenBy: new Map(),
            });
            while (state.messages.length > historyLimit) state.messages.shift();
            broadcastRoom(send, state, roomView(state));
          },
          "room.read": ({ state, event, send }) => {
            const input = event.data || {},
              member = state.members.get(input.sessionId),
              message = state.messages.find((item) => item.id === input.messageId);
            if (!member || !message || message.seenBy.has(input.sessionId)) return;
            message.seenBy.set(input.sessionId, {
              sessionId: input.sessionId,
              user: member.name,
              seenAt: new Date().toISOString(),
            });
            broadcastRoom(send, state, receiptFrame(state, message));
          },
          "room.snapshot": ({ state, event, send }) =>
            peerFrame(send, event.data?.peer, roomView(state)),
        },
      },
    },
  });
}

function makeConnection(graceMs) {
  // One logical session outlives individual sockets. Offline snapshots are
  // coalesced in JS state; the native statechart owns grace and timer cancellation.
  return defineMachine({
    id: "chat-connection",
    state: ({ input }) => ({
      kind: "connection",
      sessionId: input.sessionId,
      user: input.user,
      connectionId: input.connectionId,
      directory: input.directory,
      rooms: [],
      room: null,
      members: [],
      messages: [],
      selectedRoom: null,
      selectedVersion: 0,
    }),
    initial: "online",
    states: {
      online: {
        entry: ({ state, send, self }) => {
          send("directory.register", {
            to: state.directory,
            data: { sessionId: state.sessionId, user: state.user, peer: self },
          });
          emitCached(send, state);
        },
        on: {
          "connection.attach": {
            actions: ({ state, event, send, self }) => {
              state.connectionId = event.data.connectionId;
              send("directory.register", {
                to: state.directory,
                data: {
                  sessionId: state.sessionId,
                  user: state.user,
                  peer: self,
                },
              });
            },
          },
          "connection.detach": {
            target: "grace",
            guard: ({ state, event }) =>
              event.data?.connectionId === state.connectionId,
          },
          "connection.command": ({ state, event, send }) =>
            send("directory.command", {
              to: state.directory,
              data: { ...event.data, sessionId: state.sessionId },
            }),
          "connection.select-room": ({ state, event }) => {
            state.selectedRoom = event.data?.room ?? null;
            state.selectedVersion =
              event.data?.version ?? state.selectedVersion;
          },
          "connection.show-frame": ({ state, event, send }) =>
            applyFrame(send, state, event.data),
          "connection.show-rooms": ({ state, event, send }) =>
            applyFrame(send, state, event.data),
          "connection.show-room": ({ state, event, send }) =>
            applyFrame(send, state, event.data),
          "connection.show-error": ({ state, event, send }) =>
            applyFrame(send, state, event.data),
          "connection.error": {
            target: "grace",
            guard: ({ state, event }) => event.sendId === state.connectionId,
          },
        },
      },
      grace: {
        entry: [
          ({ state, send }) =>
            send("directory.disconnect", {
              to: state.directory,
              data: { sessionId: state.sessionId },
            }),
          ({ send }) =>
            send("connection.expire", { delayMs: graceMs, id: "grace-timeout" }),
        ],
        exit: ({ cancel }) => cancel("grace-timeout"),
        on: {
          "connection.attach": {
            target: "online",
            actions: ({ state, event }) => {
              state.connectionId = event.data.connectionId;
            },
          },
          "connection.show-frame": ({ state, event }) => applyFrame(null, state, event.data),
          "connection.show-rooms": ({ state, event }) => applyFrame(null, state, event.data),
          "connection.show-room": ({ state, event }) => applyFrame(null, state, event.data),
          "connection.show-error": ({ state, event }) => applyFrame(null, state, event.data),
          "connection.select-room": ({ state, event }) => {
            state.selectedRoom = event.data?.room ?? null;
            state.selectedVersion =
              event.data?.version ?? state.selectedVersion;
          },
          "connection.expire": "closed",
        },
      },
      closed: {
        type: "final",
        entry: ({ state, send, self }) => {
          send("directory.forget", {
            to: state.directory,
            data: { sessionId: state.sessionId },
          });
          send("host.retire-session", {
            processor: "chat-host",
            to: "host:retire-session",
            data: { sessionId: state.sessionId, actor: self },
          });
        },
      },
    },
  });
}

function applyFrame(send, state, value) {
  if (!value || !["rooms", "room", "receipt", "error"].includes(value.type)) return;
  if (value.type === "rooms") state.rooms = value.rooms;
  if (value.type === "room") {
    if (value.room !== state.selectedRoom) return;
    state.room = value.room;
    state.members = value.members;
    state.messages = value.messages;
  }
  if (value.type === "receipt") {
    if (value.room !== state.selectedRoom) return;
    const message = state.messages.find((item) => item.id === value.messageId);
    if (message) message.receipts = value.receipts;
  }
  if (value.type === "error") state.error = value;
  if (send) frame(send, state, value);
}

function createChatMachines(options = {}) {
  const graceMs = options.graceMs === undefined ? 5000 : options.graceMs;
  const historyLimit =
    options.historyLimit === undefined ? 50 : options.historyLimit;
  const maxRooms = options.maxRooms === undefined ? 16 : options.maxRooms;
  return {
    directory: makeDirectory(maxRooms),
    room: makeRoom(historyLimit),
    connection: makeConnection(graceMs),
  };
}

module.exports = { createChatMachines };
