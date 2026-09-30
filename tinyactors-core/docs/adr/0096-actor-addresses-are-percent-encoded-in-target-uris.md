# ADR 0096: Actor addresses are percent-encoded in target URIs

**Status:** Accepted and implemented (2026-09-30); see the implementation notes. Amends ADR 0044: the `#_actor_<family>/<name>` form encodes both parts.
**Date:** 2026-09-30

## Context

ADR 0044 gives a named actor the target URI `#_actor_<family>/<name>`,
and core owns both directions: `ta_scxml_target_parse()` reads it and
`ta_scxml_uri_write()` writes it, for `ta_scxml_target_format()`, for
`$_event.origin` and for an I/O processor's origin (`ta_io_origin_uri()`
goes through the same writer).

The two directions disagree for some addresses:

- **The writer puts the family and the name in as they are.**
- **The parser splits at the first `/` and refuses a second one.**
- **An address is any bytes.** `ta_address_intern()` takes any non-empty
  family and name, and so do the bindings: napi's `spawn(d, { name: 'a/b' })`
  succeeds.

So an actor named `a/b` gets the origin `#_actor_fam/a/b`, which core
refuses as a target. A reply to it is `error.execution`, and nothing can
target the actor by URI at all. A family holding `/` is worse: it would
split in the wrong place and name another address. granary's feedback
asked for names that hold `/` (the root ROADMAP).

A URI also cannot hold every byte. RFC 3986 lets a fragment hold the
unreserved characters (`A-Z a-z 0-9 - . _ ~`), the sub-delims
(`! $ & ' ( ) * + , ; =`), `:`, `@`, `/` and `?`, and anything else
percent-encoded. A name with a space or a `#` gives a text a URI parser
elsewhere (an I/O processor's client, a log reader) reads differently.

## Decision

**Both parts of `#_actor_<family>/<name>` are percent-encoded.**

- **Written:** a byte a fragment may hold is written as it is, except
  `/`, which separates the parts. Every other byte, `/` and `%` among
  them, is written `%XX` with upper-case hex digits. So a space, `#`, `"`,
  `<`, `>`, `\`, `^`, `` ` ``, `{`, `|`, `}`, control bytes and every byte
  of a non-ASCII character are encoded: `café` is `caf%C3%A9`.
- **Read:** `%XX` is decoded, in either case of hex digit. Any other byte
  but `/` is taken as it is, so a document that writes
  `#_actor_counter/c1`, or a name with a space, by hand still works. A `%`
  not followed by two hex digits is an invalid target (`error.execution`),
  as is a second unencoded `/`.
- **The family is encoded too.** It is as free as the name
  (`ta_address_intern()` accepts any bytes), and one rule for both parts
  is simpler than a rule for each. For every family and name without
  reserved bytes, the URI is what it was.
- **Parse and format stay inverses on addresses:** parsing what core
  wrote gives the same interned address, so a reply to `$_event.origin`
  reaches its sender whatever its name. The text is canonical only when
  core wrote it: `%2f` and `%2F` parse to one address, which is written
  `%2F`.
- **Session URIs do not change.** `#_scxml_<incarnation>.<n>.<family>`
  (ADR 0088) must stay `#_scxml_` followed by `_sessionid`, because
  documents build it that way (`'#_scxml_' + _sessionid`), and its
  parser already takes the family, the last part, as it is.

## Consequences

- **Any address can be targeted by URI,** and `event.origin` round-trips
  for any name.
- **A breaking change for names with reserved bytes:** an origin that
  core wrote raw is now encoded. A host or a document that built such a
  URI by concatenating the name must encode it; a binding can call core's
  writer instead of spelling the grammar again.
- **No cost for plain names.** Parsing an address without a `%` interns
  it straight from the text, as before. One with a `%` allocates a buffer
  of the decoded length from the system's allocator for the duration of
  the intern, so parsing it can also fail with `TA_ERR_NOMEM`, as
  interning already could. Writing needs no memory. No figure per actor,
  per event or per send changes.

## Implementation notes

- **Where:** `src/scxml_uri.c` (`ta_uri_keeps`, `ta_uri_put_part`,
  `ta_actor_uri_write`, `ta_uri_decode`, `ta_uri_address_intern`, replacing
  `ta_uri_address`), `src/scxml_uri.h`, the grammar's comment in
  `include/tinyactors.h`.
- **Tests:** `tests/scxml_uri_test.c` (decoding in both cases of hex, a
  hand-written raw space, `#` and UTF-8 name, an encoded and a plain
  spelling being one handle, format encoding `/`, `%`, a space, the
  characters a fragment may not hold, control bytes and UTF-8 while keeping
  the sub-delims, `:`, `@` and `?`, every encoded address parsing back to
  itself, malformed escapes invalid); the test host's `target URIs`
  scenarios (`tinyactors-test/ruby/scenarios/target_uris.rb`); napi's
  `test/origins.js` (actors named `a/b%c d`, `c/1` and `e%/1` in a family
  `echo/2` targeted, their origins encoded, and replied to with
  `toOrigin()`, on Node and Bun).
- **A public writer (not in the decision above):** `ta_actor_uri_format()`
  writes the URI of a family and a name with no system and no interned
  address, so a binding builds a target with core's encoder instead of a
  second one. napi exports it as `actorURI({ family, name })`
  (`lib/system.js`, generated binding `core.actor_uri_format`); tested in
  `tests/scxml_uri_test.c` and napi's `test/origins.js`.
