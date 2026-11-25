# values

A support desk's ticket (`desk.scxml`) and what crosses the protocol:

- its number, drawn with `Math.random` from the seed (`--seed`, 1), and the
  time it opened, `new Date()` on the host's wall clock (`--wall-start`):
  the same on every run;
- data the customer sends, given to the CRM (`--io-processor crm=...`) as it
  came: integers to 2^64-1 and down to -2^53-1 exactly, a float, text,
  `true`, `null`, `false`; bytes given as `cbor`, written back as base64url
  with their CBOR twin; `undefined` given as `cbor`, which quickjs answers
  as `null`; an address (tag 32), which a chart holds as its text;
- a send to another system, written as `message` for the client's
  transport, with the traceparent the event carried in;
- a chart no image holds (`<invoke srcexpr>`): `effect.request`, refused,
  and the invoker's `error.execution` in the same `run.turn`;
- `run.addImage` of a chart given as text, `run.spawn` of it by segment
  and index (a session past 2^32), and of a file: a new version of `desk`;
- the ticket's end with its donedata, and the forwarded `host.config`.
