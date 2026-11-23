# The live chat

The chat tutorial's charts of steps 5 and 8 (`charts/step-5` and
`charts/step-8`, copies of `examples/chat/5-hibernation/charts` and
`examples/chat/8-history/charts`, which `mise run test` checks byte for
byte) running in the reader's browser on `@tinyactors/wasm`, at the top of
https://www.tinyactors.dev/chat/. The page starts on step 8.

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
  messages on your screen; a rail of tabs with your avatar at its foot,
  whose menu signs you in or out and switches the person you are; a
  sidebar with the rooms and the people (your screen); and the room with
  each message's simulated time, who else is typing under the composer,
  and a "Connecting…" banner while your connection is lost on step 8 (on
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
`typing.stopped` that would end one step 8 started.

On step 8 a channel's history is data this program stores (`chat.mjs`,
`history`: the `chat:history` I/O processor the room chart sends each
post to). The store numbers each channel's posts and answers the room with
the number, which the room puts on the post to each member; a switch of
chart versions never starts the numbers again. A join reads the channel's
latest 50 posts from the store; the screen adds those it lacks, shown as
"earlier" since they reached it as data with no time of their own, and
shows each numbered post once. Posts made on step 5 are not in the store.
