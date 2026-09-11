# Chat example integration contract

This extends ../realtime.js into a local HTTP/WebSocket chat. The previous
deterministic transport scenario will be preserved as ../realtime-scenario.js.
Use the existing real addon through require('../..') from this directory.
All authored functions have at most three parameters. Use apply_patch. Do not
edit files assigned to another agent, the core/binding implementation, or SQLite.

## Ownership and files

- actors.js: Luna actor agent. Export createChatMachines(options) returning
  { directory, room, connection } compiled with defineMachine.
- server.js: Luna server agent. Export async startChat(options={}) returning
  { url, system, close() }. No listening on import. The host contains transport
  objects, session credentials and native actor handles, not authoritative chat
  membership/history/room-directory data.
- public/index.html, public/client.js, public/style.css: Luna client agent.
- Root owns package.json, entrypoint, tests, docs, dependency installation, and
  integration corrections. Shared protocol changes must be discussed with root.

## Actors and host integration

Exactly one directory actor with state.kind='directory', one room actor per room
with state.kind='room', and one logical connection actor per client session with
state.kind='connection'. Room membership and bounded history belong to room
actors. Directory state owns room names/actor references and routing. No socket,
Promise, callback or Node server object may be put in actor state.

createChatMachines options: graceMs=5000, historyLimit=50, maxRooms=16.
Directory spawn input: {}. Room input: {name, directory: BigInt}.
Connection input: {sessionId, user, connectionId, directory: BigInt}.
The host spawns/registers one initial room named 'lobby'.

Actor callbacks cannot reenter their System, including spawn/release. Use context
send to an explicit BigInt actor, or an I/O processor for host allocation work.
Register ioprocessors [{type:'ws'}, {type:'chat-host'}]. A WS effect is context
send('FRAME',{processor:'ws',to:'ws:'+connectionId,data:frame,id:connectionId}).
Only the host performs JSON serialization. Set send id to connectionId so late
error.communication from an obsolete transport does not detach a replacement.

Host control I/O messages (request.event.name; request.body.content.native):
- CREATE_ROOM, target 'host:create-room', data {name}. Only directory requests
  allocation after reserving the name, so duplicate concurrent creation is one
  actor. Host spawns room, then posts directory ROOM_CREATED {name,actor:BigInt}.
  On allocation failure post ROOM_FAILED {name}. Directory retains pending join
  requests until resolution; maxRooms counts reserved names too.
- RETIRE_SESSION, target 'host:retire-session', data {sessionId,actor:BigInt}.
  Host completes this lease before releasing the terminal connection actor and
  deleting its resume credentials. Defer/retry release if another lease pins it.

Directory events:
- REGISTER {sessionId,user,peer:BigInt}: register/update a connection, preserve
  its existing selected room, mark online, send its room list and current room.
- DISCONNECTED {sessionId}: mark offline; keep membership during grace.
- FORGET {sessionId}: remove session membership and watcher on expiry.
- COMMAND {sessionId,type,room?,text?}: validated client command. Supported types
  list, create, join, leave, say. At most one selected room per session.
- ROOM_CREATED {name,actor:BigInt}, ROOM_FAILED {name}, ROOM_COUNT {name,count}.
Room events: JOIN {sessionId,user,peer:BigInt}, LEAVE {sessionId},
PRESENCE {sessionId,online}, SAY {sessionId,text}, SNAPSHOT {peer:BigInt}.
Room state tracks members and bounded messages. Validate SAY membership. Use
monotonic per-room numeric message IDs; messages are {id,room,user,text}.
Membership/count changes notify directory ROOM_COUNT, which broadcasts lists.

Connection chart states: online, grace, closed(final). Online entry sends
REGISTER to directory, then emits cached room/list frames. ATTACH {connectionId}
replaces transport and returns online; grace exit cancels expiry. DETACH
{connectionId} only applies if it matches the current transport. WS communication
errors only apply if event.sendId matches the current connectionId. Entering
grace sends DISCONNECTED and starts a delayed expiry; closed sends FORGET and
RETIRE_SESSION. Offline room/list updates only replace cached snapshots.
COMMAND forwards to directory with server-owned sessionId. Directory/room push
ROOMS_VIEW/ROOM_VIEW/ERROR_VIEW to peer actors with the JSON frame as event data.
LEAVE must clear the old room view (ROOM_VIEW {type:'room',room:null,members:[],messages:[]}).

## Wire protocol

Connect WebSocket at /chat. First JSON frame:
{type:'hello',name:'Alice',resume?:'opaque-session-resume-token'}.
Server reply: {type:'welcome',session:'resume-token',sessionId:'public-id',
user:'Alice',resumed:boolean,graceMs:number}.
Resume preserves the same actor/room during grace; expiry rejects old credentials
with error code session_expired. The client then clears credentials and connects
fresh. Resuming an online session may replace its old socket; late close/message
events from the old socket must not affect the new incarnation/transport.

Client commands: {type:'list'}, {type:'create',room:'garden'},
{type:'join',room:'garden'}, {type:'leave'}, {type:'say',text:'hello'}.
Server frames:
- {type:'rooms',rooms:[{name,members:number}]}
- {type:'room',room:string|null,members:[{id,name,online:boolean}],messages:[{id,room,user,text}]}
- {type:'error',code:string,message:string}
Unknown/invalid commands produce an error, never become arbitrary actor events.
Display names: trimmed nonempty 1..32 chars. Room names: [a-z0-9_-]{1,32}.
Messages: trimmed nonempty <=2000 chars. Frames <=16KiB; reject binary frames.

## Host safety and test surface

Use node:http and ws (dependency belongs only to this example). Default bind
127.0.0.1:8080; startChat({port:0,host:'127.0.0.1',graceMs:80,historyLimit:5})
must work for integration tests. options also include maxRooms/maxSessions,
maxBufferedBytes; bounded reasonable defaults required. Unknown options need not
be supported. HTTP serves a fixed public-file allowlist, not user paths. Reject
cross-origin browser upgrades (Origin must match request Host; absent Origin is
allowed for Node clients). Generate unpredictable resume credentials in host
memory. Never log or return them from inspection. No TLS/login/database: demo
scope only. Limit payloads, handshake time, message rate and socket backpressure.

Use System.start with bounded work and an onIO drain notification. Process host
effects outside actor callbacks. Every IO lease must complete exactly once on
write callback/failure/timeout/shutdown; one slow socket must not block all peers.
When >one batch remains, schedule another drain. close() stops timers/transports,
settles leases, and destroys the system; returns a Promise and is idempotent.
Server should expose its real system for trusted in-process streaming tests.

## Browser client

Native WebSocket, no build/runtime frontend dependency. Join/create/leave room,
room list, members/presence, history and sending messages. Keep resume credentials
in sessionStorage (per tab); reconnect with bounded exponential backoff. Offline
UI must not pretend sends succeeded. Render untrusted text with textContent, not
innerHTML. The demo must be usable in multiple tabs/browsers on localhost.
