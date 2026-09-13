# Realtime actor chat

A real, local multi-client chat using the native Tinyactors addon, Node HTTP,
and WebSockets. The browser client has no framework or build step.

## Run

From the repository root:

```sh
npm --prefix bindings/napi run build
npm --prefix bindings/napi/examples/chat ci
npm --prefix bindings/napi/examples/chat start
```

To run the chat server with Jaeger and view actor traces, use Docker Compose:

```sh
docker compose -f bindings/napi/examples/chat/docker-compose.yml up --build
```

For live development, use Compose Watch:

```sh
docker compose -f bindings/napi/examples/chat/docker-compose.yml watch
```

Changes under `public/` sync immediately; JavaScript changes restart the chat
container; native/core, dependency, Dockerfile changes trigger an image rebuild.

Open the chat at <http://127.0.0.1:8080>, then view traces at
<http://127.0.0.1:16686>. The example exports OTLP/HTTP traces to Jaeger;
`OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` or `OTEL_EXPORTER_OTLP_ENDPOINT` can be
used to point it at another collector. Message payload attributes are disabled
by default; enable them for local debugging with
`OTEL_TINYACTORS_CAPTURE_PAYLOAD=true`.

Open <http://127.0.0.1:8080> in two separate tabs, choose different display names,
and join **lobby**. Create another room to try room isolation. Creating an existing
room joins it; each session belongs to at most one room.

Click **Disconnect** in one tab, send messages from the other, then click
**Reconnect** within five seconds. The same connection actor resumes with its
room membership and latest bounded history. A lost connection reconnects
automatically with exponential backoff (400 ms to 10 s). Page reload also attempts
resume, unless you deliberately disconnected. After the grace period expires,
the client starts a fresh session and must join a room again.

`PORT=9000` selects another port; `PORT=0` selects an available port. `HOST`
defaults to `127.0.0.1`. Ctrl-C stops the server and closes its native system.
The original deterministic transport demonstration remains available:

```sh
node bindings/napi/examples/realtime-scenario.js
```

## What is an actor?

| Actor      | Count                          | Authoritative JavaScript state                                           |
| ---------- | ------------------------------ | ------------------------------------------------------------------------ |
| Directory  | Exactly one                    | Room names and actor IDs, pending room reservations, session routing     |
| Room       | One per room, including lobby  | Members and presence, bounded message history, per-room message sequence |
| Connection | One per logical client session | Current transport ID, selected room, cached views, reconnect lifecycle   |

[actors.js](actors.js) defines all three machines with nested JavaScript objects.
Room and directory actors exchange messages with connection actors; they never
write to sockets. Their state consists of native JS objects, arrays and Maps.
Names such as `__proto__` are ordinary room names, not object property lookups.

The connection chart is `online → grace → closed`, with `connection.attach` returning
`grace → online`. Entering grace marks presence offline and schedules expiry;
leaving grace cancels that timer. Offline updates replace cached snapshots, not
an unbounded message backlog. Expiry removes membership and releases the actor.
Resume credentials and public session IDs are distinct; credentials never enter
actor state. Transport IDs also change on every WebSocket connection. Late
close/write-failure events from an old socket cannot detach its replacement.

[server.js](server.js) owns sockets, credentials and native resource lifetimes.
It implements two I/O processors:

- `ws`: serialize a frame and complete its lease on write success, failure,
  timeout or shutdown. This completion acknowledges a local transport write,
  **not** delivery to a browser.
- `chat-host`: allocate a reserved room, or release an expired connection actor.
  Allocation and release happen outside actor callbacks, avoiding native-system
  reentry. A retirement lease is completed before releasing its origin actor.

The directory reserves a name before requesting allocation, so simultaneous
creates produce one actor. A pending join has a generation: delayed allocation
cannot undo a subsequent leave or selection. Reservations count toward the room
limit even when all waiting clients have cancelled. Empty rooms remain until
server shutdown; there is no room-deletion protocol.

The [integration contract](CONTRACT.md) lists actor events and the JSON protocol.
Only validated `list`, `create`, `join`, `leave` and `say` commands reach actors;
clients cannot inject arbitrary internal events.

## Embedding and inspection

```js
const { startChat } = require("./bindings/napi/examples/realtime");

async function main() {
  const chat = await startChat({ port: 0, graceMs: 10000, historyLimit: 100 });
  console.log(chat.url);
  // Trusted, in-process streaming inspection; no public inspection endpoint.
  let page;
  do {
    page = chat.system.inspect(
      (record) => {
        if (record.kind === "actor") {
          console.log(record.actor, record.info.state.kind);
        }
      },
      { cursor: page?.cursor, workUnits: 32 },
    );
  } while (!page.done);
  // Keep the server alive while your application uses it.
  // When finished: await chat.close();
}
main().catch(console.error);
```

`startChat()` returns `{ url, system, close, errors }`. Importing it does not
listen. `close()` is asynchronous and idempotent. The `system` is exposed for
trusted inspection; do not mutate its actor state or run a second scheduler.
Unexpected host/native errors are recorded (up to 32), reported through
`onError(error)` or a Node warning, and shut down the server. Expected invalid
commands and transport failures are handled without stopping other clients.

### Limits

All numeric options are validated. The defaults are demo limits, not claims
about the core engine's capacity.

| Option             | Default | Allowed range / meaning                                         |
| ------------------ | ------- | --------------------------------------------------------------- |
| `port`             | 8080    | 0–65535                                                         |
| `graceMs`          | 5000    | 1–600000; retain disconnected sessions                          |
| `historyLimit`     | 50      | 1–1000 messages per room                                        |
| `maxRooms`         | 16      | 1–1000, including lobby and reservations                        |
| `maxSessions`      | 128     | 1–10000, including grace/retiring sessions                      |
| `maxBufferedBytes` | 1048576 | 1024–67108864; queued bytes plus next outgoing frame per socket |
| `heartbeatMs`      | 15000   | 20–300000; terminate after a missed pong interval               |
| `handshakeMs`      | 10000   | 20–60000; time to send a valid hello                            |
| `writeTimeoutMs`   | 5000    | 20–30000; maximum outstanding socket-write time                 |
| `rateLimit`        | 60      | 1–10000 incoming frames per second per physical connection      |

Incoming frames are at most 16 KiB, text only. Display names are 1–32 characters;
room names match `[a-z0-9_-]{1,32}`; trimmed messages are 1–2000 characters.
Outgoing snapshots can exceed 16 KiB but must fit `maxBufferedBytes`. Choose that
limit to accommodate your history and membership settings: an oversized snapshot
disconnects its recipient just like backpressure. At most twice `maxSessions`
WebSockets may be open (including incomplete handshakes and replaced sockets).
The native system caps queued events at 65536 and I/O requests at 16384; scheduler
and I/O drain batches yield to Node. Resource exhaustion during an actor action
can stop the demo rather than guarantee recovery or delivery.

## Scope and safety

This is an **in-memory localhost example**, not a deployable public chat service.
Display names are not authenticated, room names are not access controls, and
resume tokens are bearer credentials. Do not share tokens. No TLS, user accounts,
database, moderation, distributed coordination or durable delivery is provided.
No message deduplication/acknowledgement protocol is claimed: a disconnect can
lose a submitted command, and history older than the configured window is lost.

The host serves only fixed static files, refuses cross-origin browser upgrades,
limits socket payloads and queues, and disables WebSocket compression. The UI
renders user text with `textContent`. These controls do not make the example safe
to expose publicly. The `ws` dependency belongs only to this example; the binding
and libc-free C engine still have no transport/npm runtime dependency. See the
primary [ws documentation](https://github.com/websockets/ws) for its server,
ping/pong and write-callback interfaces.

## Verify

```sh
npm --prefix bindings/napi/examples/chat test
npm --prefix bindings/napi test
```

The chat suite exercises the real addon and real localhost WebSockets: actor
counts, multi-client delivery, room isolation, reservation races, bounded history,
offline snapshots, resume/takeover/expiry, stale events, admission and frame
limits, heartbeat loss, backpressure, startup failure and cleanup. Deterministic
actor tests also hold host allocation leases to force pending-create races.
