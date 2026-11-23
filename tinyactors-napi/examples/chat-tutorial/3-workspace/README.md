# Step 3: the workspace

In step 2 your program spawned the room. Now a workspace actor makes the
rooms. It is a collection actor: it decides which rooms exist, and it
alone spawns them.

## Making a room

The package's `Spawner` is an I/O processor that spawns actors. The
program gives it to the host under the type `spawn`:

```js
const host = new Host(compiled, {
  system: 'chat',
  ioProcessors: { 'chat:client': client, spawn: new Spawner() },
});
```

The workspace sends it the chart, the name and the params, one
`<param>` each. It adds the name to `rooms` at once. `idlocation` stores
the send's id, and the workspace remembers who asked under that id:

```xml
<transition event="room.create" cond="refusal(rooms, _event.data.name) === null">
  <send type="spawn" event="spawn" idlocation="sendid">
    <param name="chart" expr="'room'"/>
    <param name="name" expr="'room.' + _event.data.name"/>
    <param name="params" expr="({ name: _event.data.name })"/>
  </send>
  <assign location="rooms" expr="rooms.concat([_event.data.name])"/>
  <assign location="creating[sendid]" expr="({ name: _event.data.name, by: _event.origin })"/>
</transition>
```

The `Spawner` answers `spawned` with the same sendid, and the workspace
answers the asker. The answer comes as a new event, after any request
already waiting. A second request for the same name may come first, and
it finds the name in `rooms`. So no name is spawned twice.

`refusal` is a function of a chart-level `<script>`. The chart's
expressions call it. It takes the variables it reads as arguments. A
name is 1 to 32 lowercase letters, digits and dashes, and starts with a
letter or digit, so `-` is refused.

## Asking an actor

The program asks the workspace with `actor.ask`. The workspace answers
to `_event.origin`, and `ask` returns the answer as `[event, data]`:

```js
const [event, data] = await workspace.ask('room.create', { name: 'general' });
```

## Joining a room that does not exist

The user actor now joins rooms and posts to them. It sends to the room
by name. A send to a name that no actor has fails at once: the sender
gets `error.communication` with the send's id.

```xml
<transition event="join">
  <send id="join" targetexpr="'tinyactors://chat/@room.' + _event.data.room" event="join">
    <param name="user" expr="name"/>
  </send>
  <raise event="activity"/>
</transition>
<transition event="error.communication" cond="_event.sendid === 'join'">
  <send type="chat:client" event="error">
    <param name="to" expr="name"/>
    <param name="reason" expr="'no such room'"/>
  </send>
</transition>
```

A join or a post counts as activity. The transition raises `activity`,
and `active` restarts its timer.

## Run it

```sh
node examples/chat-tutorial/3-workspace/main.mjs
```

```output
create general: room.created
create general: room.refused (exists)
create Bad Name: room.refused (invalid name)
create -: room.refused (invalid name)
rooms: general
ada joined general
bob gets an error: no such room
bob joined general
ada sees ada in general: hi
bob sees ada in general: hi
```

Actors run in no fixed order relative to each other, so lines of
different users can swap.

Next: [The status roster](../4-roster/).
