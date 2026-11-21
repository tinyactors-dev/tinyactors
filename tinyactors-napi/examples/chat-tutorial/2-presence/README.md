# Step 2: presence

Each user is now an actor too. A user is offline, active or away. An
active user turns away after 300 seconds without activity.

[charts/user.scxml](charts/user.scxml) has the three states. `online`
holds `active` and `away`, so a `disconnect` leaves either one:

```xml
<state id="online" initial="active">
  <transition event="disconnect" target="offline"/>
  <state id="active">
    <onentry><send id="idle" event="idle" delay="300s"/></onentry>
    <onexit><cancel sendid="idle"/></onexit>
    <transition event="activity">
      <cancel sendid="idle"/>
      <send id="idle" event="idle" delay="300s"/>
    </transition>
    <transition event="idle" target="away"/>
  </state>
  <state id="away">
    <transition event="activity" target="active"/>
  </state>
</state>
```

The `idle` event is a delayed send to the actor itself. Activity cancels
it and sends a new one. Each state also reports itself to the client
processor in its `<onentry>`.

The room no longer talks to your program. It sends each post to each
member's user actor, by name:

```xml
<foreach array="members" item="member">
  <send targetexpr="'tinyactors://chat/@user.' + member" event="message">
    <param name="from" expr="_event.data.user"/>
    <param name="text" expr="_event.data.text"/>
  </send>
</foreach>
```

A user actor shows a message only in `online`. An offline user has no
transition for `message`, so the event is dropped.

[main.mjs](main.mjs) spawns each user under the name `user.<name>`. It
runs on a `SimulatedClock`, so 300 seconds take no time:

```js
const clock = new SimulatedClock();
const host = new Host(compiled, { system: 'chat', clock, ioProcessors: { 'chat:client': client } });
// ...
clock.increment(300_000);
await host.idle();
```

```output
ada is offline
bob is offline
ada is active
bob is active
-- 300 seconds pass
ada is away
bob is away
-- bob types, ada closes the app
bob is active
ada is offline
bob sees bob: anyone?
```

Next: [The workspace](../3-workspace/).
