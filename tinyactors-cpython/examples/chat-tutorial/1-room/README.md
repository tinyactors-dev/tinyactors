# Step 1: a room

A room is an actor. It keeps a list of members. Your program sends it
events, and it sends each post out to your program.

[charts/room.scxml](charts/room.scxml):

```xml
<state id="open">
  <transition event="join">
    <assign location="members" expr="members + [_event.data['user']]"/>
  </transition>
  <transition event="post">
    <send type="chat:client" event="message">
      <content expr="{'to': members, 'from': _event.data['user'], 'text': _event.data['text']}"/>
    </send>
  </transition>
</state>
```

[main.py](main.py) compiles the chart, starts one room and sends it
three events:

```python
room = host.spawn("room", name="general")
room.send("join", {"user": "ada"})
room.send("join", {"user": "bob"})
room.send("post", {"user": "ada", "text": "hi"})
host.run_until_idle()
```

The `<send type="chat:client">` goes to the I/O processor of that type.
The program gives the host one, an object with `receive(send)`. Here it
stands in for the users' screens:

```python
class Client:
    def receive(self, send):
        for user in send.data["to"]:
            self.lines.append(f"{user} sees {send.data['from']}: {send.data['text']}")
```

```sh
python main.py
```

```
ada sees ada: hi
bob sees ada: hi
```

The room does no I/O. It decides who gets a message, and your program
delivers it.

Next: [Presence](../2-presence/).
