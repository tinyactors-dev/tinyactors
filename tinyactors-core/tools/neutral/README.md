# Language-neutral charts: json beside staticjs, with the app's functions in Swift

A spike on a branch. The same charts written for two neutral datamodels,
`json` (paths, literals, the app's functions) and `staticjs` (an ECMAScript
subset), with the app's functions written once, in Swift, and registered
with either datamodel through the one ABI both take (`ta_json_call`; the
staticjs datamodel gained `ta_datamodel_staticjs_register_json` for it).

```swift
functions.register("crossed") { (rule: Rule, quote: Quote) -> Bool in
    quote.symbol == rule.symbol && quote.price >= rule.above
}
```

The library decodes the arguments into the closure's parameter types and
encodes its result (`swift/Sources/Tinyactors/Functions.swift`); the app
never sees bytes. `run.sh` builds the Swift side and runs both programs with
both datamodels.

## The market tracker: a system of three charts

`charts/<datamodel>/tracker.scxml` invokes `watchlist.scxml`, which invokes
`alerts.scxml`; compiling the tracker brings the other two into its image.

| Chart | What it decides |
|---|---|
| `tracker` | whether the device has a network (`network.offline`, `network.online`, from the OS's reachability: `NWPathMonitor`, `ConnectivityManager`) and the feed's connection: `checking`, `connecting`, `live`, `waiting` (a lost feed, retried with a backoff that doubles to 30 s), `suspended` (offline) |
| `watchlist` | the quotes and whether they are fresh: `live` with the feed, `stale` from the moment it is not, their last price and time kept |
| `alerts` | price rules, each firing once; a crossing a reconnect's snapshot shows fires as late ("while you were offline") |

How airplane mode is handled:

- **Offline suspends the feed:** no reconnect attempt and no backoff timer
  until the network is back (the app's `connect` count shows it), then
  one attempt at once with the backoff reset. A lost feed while online
  backs off.
- **The socket's late error is ignored:** it arrives after
  `network.offline`, in `suspended`, which has no transition for it.
- **The subscriptions are state, not a queue:** a symbol added offline
  joins the list, and every connection subscribes to the whole list.
- **A reconnect's snapshot fills the gap:** the watchlist takes it, and
  the alerts evaluate it, so a crossing during the outage is reported.
- **Staleness is a state:** the report says which quotes are stale.

`swift/Sources/Market/main.swift` is the app's side on a simulated clock:
the feed (a WebSocket in a real app), the device's network, the UI. Both
datamodels print the same run.

## What the comparison shows

| | json | staticjs |
|---|---|---|
| Lines that differ between the two versions (`diff`, the `datamodel` attribute and comments included) | tracker 10 of 86, watchlist 3 of 34, alerts 4 of 42, order 10 of 35 | |
| Functions the app writes, market | 6: `ruleFrom`, `crossed`, `summary`, and `due`, `ms`, `nextBackoff` instead of operators | 3 |
| Functions the app writes, order | 4: `knows`, `priced`, and `placeable`, `plusLine` instead of operators | 2 |
| A mistake in an expression | the chart's load refuses a path or literal it cannot read | `!=` (the subset has `!==` only) was `error.execution` at its first evaluation |
| Datamodel bytes per actor, 10,000 orders, idle / after the events | 464 / 818 | 1,073 / 2,512 |
| ns per event, `address` (no call) | 1,756 to 1,774 | 2,262 to 2,430 |
| ns per event, `add` (two Swift calls) | 9,083 to 9,306 | 11,109 to 11,450 |
| The Swift side of one call (CBOR read into `Request`, `Item` written) | 3,600 ns | the same, plus staticjs's conversion |
| The market system's datamodel, three actors | 75 KB | 140 KB |

(`Order json|staticjs`, release builds, one machine, two runs each; best of
five rounds of 10,000 events.)

- **The charts behave the same,** and differ only where json has no
  operator: a string built for a log, a delay, an arithmetic step, an
  append, a test of two fields.
- **What json lacks moves into the app:** `nextBackoff` and `due` are the
  chart's policy, written in Swift, where a reader of the chart does not
  see them.
- **The Swift codec, not the datamodel, is the cost of a call:** 3.6 µs of
  the 3.7 µs a call adds. This spike's coder builds a tree; a binding
  would stream.
- **json holds about a third of staticjs's memory per actor.**

## What it leaves out

The order's `place` and the market's reports log through `<log>`; no
hibernation or migration is exercised here (both datamodels pass the
suite's scenarios for them, `mise run conform`). The Swift host is the
spike's, not a binding.
