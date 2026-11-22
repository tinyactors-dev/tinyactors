# The live chat

The chat tutorial's finished charts (`charts/`, a copy of
`examples/chat/5-hibernation/charts`, which `mise run test` checks byte for
byte) running in the reader's browser on `@tinyactors/wasm`, at the top of
https://www.tinyactors.dev/chat/.

- `chat.mjs` is what it does: the workspace, two rooms and six people on a
  host with a simulated clock and a store that keeps at most four actors in
  memory. Each person's screen is built only from what the charts send to
  `chat:client` for that person.
- `main.mjs` runs it in Node.js, the page's buttons in order, and prints
  what `output.txt` holds:

  ```sh
  npm ci
  node main.mjs
  ```

- `page.mjs` draws it: who you are, the rooms and the people (your
  screen), the room, and any actor's chart lit in the state its host
  reports (`components/live-chart.mjs`), with the clock's controls.

The roster a person's screen holds leaves out their own changes (the
workspace sends a change to the other people online), so the page shows
your own status from your actor's state.
