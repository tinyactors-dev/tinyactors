# The live chat

The chat tutorial's charts of steps 5 and 9 (`charts/step-5` and
`charts/step-9`, copies of `examples/chat/5-hibernation/charts` and
`examples/chat/9-direct-messages/charts`, which `mise run test` checks byte for
byte) running in the reader's browser on `@tinyactors/wasm`, at the top of
https://www.tinyactors.dev/chat/. The page starts on step 9.

- `chat.mjs` is what it does: the workspace (The Hill), two rooms and six people (an ant colony's
  castes: queen, scout, forager, nurse, guard, drone) on a
  host with a simulated clock and a store that keeps at most four actors in
  memory. Each person's screen is built only from what the charts send to
  `chat:client` for that person. Its experiments are what an application
  around the charts does to them: a lost connection that comes back later
  (the connections are the application's, as a page's sockets: a person
  signed out has none to lose),
  a screen that drops or delays what reaches it, two scripted scenarios on
  the simulated clock's agenda, a log of what happened, and a switch
  between the two versions by a live migration of every actor.
- `main.mjs` runs it in Node.js, the page's buttons in order, and prints
  what `output.txt` holds:

  ```sh
  npm ci
  node main.mjs
  ```

- `page.mjs` draws it as a team chat client looks (`.chatlive` in
  `style/site.css`, in Lato): a top bar whose search filters the room's
  messages on your screen; a rail of views (Home, DMs, Activity) with
  your avatar at its foot, whose menu signs you in or out and switches
  the person you are; in the sidebar's place, the view picked: Home's
  rooms, direct messages and people (your screen; a person opens your
  direct message with them, yourself your notes), DMs' list of your
  direct messages with each one's last message, or Activity's posts that
  mention you and direct messages to you; and the room or direct message with
  each message's simulated time, who else is typing under the composer,
  and a "Connecting…" banner while your connection is lost on step 9 (on
  step 5 you are signed out). The Debugger button in the top bar opens a
  pane with the clock's controls and three tabs: Experiments (the charts'
  version, the scenarios, and per person a lost connection and the
  screen's faults), Actors (every actor, in memory or in the store, and
  any actor's chart, of the version running, lit in the state its host
  reports: `components/live-chart.mjs`) and Log (the newest entries first,
  filtered by kind).

The roster a person's screen holds leaves out their own changes (the
workspace sends a change to the other people online), so the page shows
your own status from your actor's state. While your actor is in the
store, its state is in its blob, so the page shows what the workspace
last told another person online about you.

On step 5 the page shows no typing indicator: those charts send none,
and a switch to them clears the indicators, since they never send the
`typing.stopped` that would end one step 9 started.

Step 5 has no `reconnecting` state either. A live migration would put a
person whose connection is lost in step 5's initial state, `offline`,
without running its entry actions, so the workspace would never hear of
it and everyone's roster would still show them active. So before a
switch to step 5 the program ends each lost connection as an application
on step 5's charts would have: it sends `disconnect` (step 9's chart
then goes to `offline` and tells the workspace), keeps the reconnect the
agenda holds, which signs the person in again, and says so in the log.
A switch to step 9 needs nothing for it: on step 5 a lost connection is
offline already.

On step 9 a channel's history is data this program stores (`chat.mjs`,
`history`: the `chat:history` I/O processor the room chart sends each
post to). The store numbers each channel's posts, records when each was
made by the simulated clock, and answers the room with both, which the
room puts on the post to each member; a switch of chart versions never
starts the numbers again. A join reads the channel's latest 50 posts from
the store; the screen adds those it lacks and shows each numbered post
once. A screen shows each post at the time the store recorded, whether it
came live, held during a lost connection, delayed by a fault, or read
from the store. Step 5's posts are not stored and carry no time, so on
step 5 a screen shows when each post reached it.

A screen also misses posts: those made while its person was signed out or
offline, and deliveries a fault dropped. Catching up is the program's
job, not a chart's. When a person's connection opens, their screen reads
from the store, for each room it joined, the posts after its newest
number. When a message arrives more than one number ahead, the screen
reads the gap first. Posts made on step 5 are not in the store, so on
step 5 a screen catches up on nothing.

Step 9 adds direct messages: a room of two that the workspace makes once
per pair (`openDM`), named `dm.` and both names, which no channel name
can be. The asker's screen hears `dm.opened`, and each member's screen
hears `dms`, their direct messages, when they come online or one is made
with them. A screen reads each from the store as it reads a channel it
joins, and lists it once its person opened it or a post came
(`directMessages`, newest first, each with its last message). Step 5's
charts have none: the page hides the list, and a click on a person says
so. Step 5's workspace keeps no direct messages and its room no `direct`,
so before a switch to step 5 the program ends each direct message's room,
its posts kept in the store, and after the switch back the workspace makes
each again.

Activity is read from the screen too: the posts on it that someone else
made in your direct messages, and those that mention you as `@` and your
name (a bare name is too often a word: "the forager trail"). An item is
unread until its conversation showed it, you opened it, or you marked all
read. Team chat clients put saved items, files and canvases under More;
this chat has none of them, so the rail has no More.
