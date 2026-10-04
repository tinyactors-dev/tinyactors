# Expressions as a statechart's data model evaluates them: the chart's data
# and _event are globals, and the shared functions come from a module.

=== the campaign's helpers
@module campaign
export const take = (xs, n) => xs.slice(0, n);
export const drop = (xs, n) => xs.slice(n);
export const without = (ids, id) => ids.difference(new Set([id]));
export const unsent = (batch, pending, sent) => batch.filter(n => pending.has(n.id) && !sent.has(n.id));
export const tracker = id => `#_actor_progress/${id}`;
--- value
{"drop":<function>,"take":<function>,"tracker":<function>,"unsent":<function>,"without":<function>}

=== a guard on the event
@global _event = {name: "notify.ack", data: {id: "n2"}}
@global pending = new Set(["n1", "n2"])
pending.has(_event.data.id) && _event.name === "notify.ack"
--- value
true

=== the window of an attempt
@global batch = [{id: "a"}, {id: "b"}, {id: "c"}, {id: "d"}, {id: "e"}]
@global pending = new Set(["a", "c", "d", "e"])
@global windowSize = 2
batch.filter(n => pending.has(n.id)).slice(0, windowSize).map(n => n.id)
--- value
["a","c"]

=== a counter, as an assign would write it
@global counts = {sent: 4, acked: 1, rejected: 0, failed: 0}
({...counts, acked: counts.acked + 1})
--- value
{"sent":4,"acked":2,"rejected":0,"failed":0}

=== removing an acknowledged ID
@global pending = new Set(["n1", "n2", "n3"])
@global _event = {data: {id: "n2"}}
pending.difference(new Set([_event.data.id]))
--- value
new Set(["n1","n3"])

=== an eventless transition's guard
@global pending = new Set()
pending.size === 0
--- value
true

=== the reply when the campaign gives up
@global pending = new Set(["x", "y"])
@global attempt = 6
@global maxAttempts = 5
attempt > maxAttempts ? {event: "failed", pending: [...pending]} : null
--- value
{"event":"failed","pending":["x","y"]}

=== a send's target
@global campaignID = "c-42"
`#_actor_progress/${campaignID}`
--- value
"#_actor_progress/c-42"

=== a missing field reads as undefined, and ?? gives a default
@global _event = {data: {}}
_event.data.priority ?? 1
--- value
1

=== optional chaining over event data that may be absent
@global _event = {name: "tick"}
_event.data?.id ?? "none"
--- value
"none"
