# Step 4: the status roster

The workspace now keeps a roster: each user's status. A user who comes
online gets the whole roster. The other online users get each change.

## Users report to the workspace

Each user state sends its status to the workspace instead of the client:

```xml
<send target="tinyactors://chat/@workspace" event="status">
  <content expr="{'user': name, 'status': 'away'}"/>
</send>
```

## The workspace keeps the roster

```xml
<transition event="status">
  <assign location="arriving" expr="came_online(roster, _event)"/>
  <assign location="roster[_event.data['user']]" expr="_event.data['status']"/>
  <if cond="arriving">
    <send targetexpr="_event.origin" event="roster"><content expr="{'roster': roster}"/></send>
  </if>
  <foreach array="online(roster, _event.data['user'])" item="user">
    <send targetexpr="'tinyactors://chat/@user.' + user" event="roster.update">
      <content expr="_event.data"/>
    </send>
  </foreach>
</transition>
```

`came_online` and `online` are in
[charts/workspace.py](charts/workspace.py). The user actor passes
`roster` and `roster.update` on to the client.

A user reports a status only when it changes. In step 2 activity
already restarts the timer in a transition without a target: `active`
stays, and its `<onentry>` does not run again. With a target, each post
would send the roster a new `active`.

## Run it

```sh
python main.py
```

```
-- ada connects
ada sees the roster: ada active, bob offline, cat offline
-- bob connects
bob sees the roster: ada active, bob active, cat offline
ada sees bob is active
-- 300 seconds pass
bob sees ada is away
ada sees bob is away
-- bob closes the app
ada sees bob is offline
the workspace answers: {'ada': 'away', 'bob': 'offline', 'cat': 'offline'}
```

Next: [Hibernation](../5-hibernation/).
