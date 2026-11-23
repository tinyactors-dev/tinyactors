# The live chat

The chat tutorial's finished charts (`charts/`, a copy of
`examples/chat/5-hibernation/charts`, which `mise run test` checks byte for
byte) running in the reader's browser on `@tinyactors/wasm`, at the top of
https://www.tinyactors.dev/chat/.

- `chat.mjs` is what it does: the workspace (The Hill), two rooms and six people (an ant colony's
  castes: queen, scout, forager, nurse, guard, drone) on a
  host with a simulated clock and a store that keeps at most four actors in
  memory. Each person's screen is built only from what the charts send to
  `chat:client` for that person.
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
  each message's simulated time. The bug button in the top bar opens a
  debugger pane with the clock's controls, every actor, in memory or in
  the store, and any actor's chart lit in the state its host reports
  (`components/live-chart.mjs`).

The roster a person's screen holds leaves out their own changes (the
workspace sends a change to the other people online), so the page shows
your own status from your actor's state. While your actor is in the
store, its state is in its blob, so the page shows what the workspace
last told another person online about you.
