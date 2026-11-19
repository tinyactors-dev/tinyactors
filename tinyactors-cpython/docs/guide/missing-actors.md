# Sending to an actor that is not there

An application with many actors sends to actors that do not exist at
that moment: a room nobody has opened yet, a user with no connection
open, an order that has ended. What should happen then is part of the
application's logic, and it depends on the kind of actor. The library
has no policy of its own and no hook for it. It gives you three ways,
each built on calls you already use, and an application usually mixes
them, one per kind of actor:

| Way | Choose it when | What it costs |
|---|---|---|
| A. The actor never ends, and hibernates | the actor is a long-lived thing your application names (a workspace, a room, an account) | a blob in your store and a record in the host per actor |
| B. The application routes it | whether the actor exists is known outside the charts (who is online), or a miss needs I/O (store the message, notify) | an I/O processor |
| C. The sender handles `error.communication` | the sender can tell its own client (no such room) or try something else | a transition in the sender |

The chat example ([examples/chat](../../examples/chat/)) uses all
three: its README says where.

## A. The actor never ends, and hibernates

A chart with no top-level `<final>` that its application never
terminates lives as long as the host. That costs little: with a store,
the host keeps at most `resident` actors in memory and the rest as blobs
in the store, and when an event comes for a hibernated actor, by its
session or by its name, the host loads its blob and wakes it before the
event runs: an event for a hibernated actor asks for its wake
(`ta_system_make_runnable`, the core's `src/system/system.c:553-555`),
which the host hands to the store as a `TA_EFFECT_WAKE`
(`ta_system_wake_request`, `src/system/hibernate.c:1438`). The sender sees no difference.

```xml
<!-- room.scxml: no <final>; it waits for events as long as the host runs -->
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       name="room" datamodel="python" initial="open">
  <datamodel><data id="members" expr="{}"/></datamodel>
  <state id="open">
    <transition event="join">
      <assign location="members" expr="{**members, _event.origin: _event.data['user']}"/>
    </transition>
  </state>
</scxml>
```

```python
store = tinyactors.SQLiteStore("actors.db")
host = tinyactors.Host(compiled, system="chat", store=store, resident=10_000)
host.spawn("room", name="room.acme.general")      # once; it is there from then on
```

A chart then sends to `tinyactors://chat/@room.acme.general`, and it
reaches the room whether it is in memory or in the store.

- **The store is not durability.** Its blobs wake only in the host that
  wrote them (or one restored from that host's snapshot). What must
  survive a restart belongs in your database, and the application spawns
  the actors again from it at startup. In the chat, a room saves itself
  when it starts and announces itself to its workspace, so the startup
  code spawns the rooms and nothing else.
- **Not every actor can hibernate.** One that is not at rest (events
  waiting, an invocation running), or a `"python"` actor holding a value
  outside the value table (a `set`, a `tuple`, an object), stays in
  memory.
- **`actor_state` and `actor_context` refuse a hibernated actor.** Ask it
  with an event instead (`await actor.ask(...)`), which wakes it.

## B. The application routes it

The chart sends to an I/O processor of yours, and the processor decides
where the event goes. It can look the actor up (in its own tables, or
with `host.actor(name)`, which raises `StaleError` when none answers),
forward the event with `send.host.send(...)`, and do something else when
there is none: store the message, notify someone, answer the sender.

In the chat a direct message goes to a user, who is not an actor: the
user's connections are, and there may be none. The connection's chart
hands the message to the `chat:inbox` processor:

```xml
<transition event="client.dm">
  <send type="chat:inbox" event="dm">
    <content expr="{'from': user, 'to': _event.data['to'], 'text': _event.data['text']}"/>
  </send>
</transition>
<transition event="dm">                <!-- forwarded by the inbox -->
  <send type="chat:gateway" event="deliver"><content expr="frame('dm', _event.data)"/></send>
</transition>
```

```python
class Inbox:
    def __init__(self, gateway, database):
        self.gateway, self.database = gateway, database

    def receive(self, send):
        delivered = 0
        for session in self.gateway.connections(send.data["to"]):
            try:
                send.host.send(session, "dm", send.data)
                delivered += 1
            except tinyactors.StaleError:         # it ended meanwhile
                self.gateway.detach(session)
        if not delivered:
            self.database.add_unread(send.data)   # read when the user connects
```

When the missing actor should exist, keep the decision in a chart: the
processor (or the sender) asks the collection actor that owns that kind
of actor (a workspace for its rooms), and the collection actor creates
it, with the `Spawner` processor, and answers. A processor that spawns
on its own decides what exists outside the charts.

## C. The sender handles `error.communication`

A `<send>` to a local address that no actor answers to fails at once:
the sender gets `error.communication`, with the send's `sendid` in
`_event.sendid`, at once. A send to another system fails
the same way when your transport calls `send.fail()`, or later when it
calls `host.fail(origin, sendid)`. Give the send an `id`, and handle the
error in the state that sent it:

```xml
<transition event="client.join">
  <send id="join" targetexpr="room_address(workspace, _event.data['room'])" event="join">
    <content expr="{'user': user}"/>
  </send>
</transition>
<transition event="error.communication" cond="_event.sendid == 'join'">
  <send type="chat:gateway" event="deliver">
    <content expr="frame('error', {'reason': 'no such room'})"/>
  </send>
</transition>
```

A chart that does not handle it ignores it, as any event no transition
takes: the send is lost, quietly. Handle it wherever a lost send
matters.

## Names that need no encoding

An actor's name is part of its address, `tinyactors://<system>/@<name>`,
and in an address a name is RFC 3986's unreserved characters, `A`-`Z`,
`a`-`z`, `0`-`9`, `-`, `.`, `_` and `~`, or percent-encodings of other
bytes (the core's `src/base/address.c:18-21`, checked at `:76`;
`include/tinyactors/address.h:13-14`). `Address.from_name(system,
name)` encodes any name, but a name like `room/general` or
`room:acme:general` is then `room%2Fgeneral` or `room%3Aacme%3Ageneral`
in every chart, log line and test that writes it: a `/` or `:` needs
encoding.

Name actors with unreserved characters only, and separate the parts with
`.`: `workspace.acme`, `room.acme.general`. Then a chart writes the
address as plain text:

```python
# connection.py
def room_address(workspace, room):
    return "tinyactors://chat/@room." + workspace + "." + room
```

and the application refuses a name with other characters where it is
chosen, as the chat's workspace refuses a room name outside
`a`-`z`, `0`-`9` and `-` (`valid_name` in `charts/workspace.py`).
