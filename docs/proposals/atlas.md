# Atlas: core's types and calls from DWARF, and a snapshot's memory, seen

**Status:** Sketch (2026-11-25), from a discussion with the maintainer.
**Touches:** tinyactors-core (`mise/dwarf.rb`, a new `mise run atlas`,
`tinyactors inspect`, the snapshot pack's header), website (a viewer).
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Whom it serves, and who pays

First the **maintainers** and newcomers to core, who ask what a struct is,
how big, and who touches it. Then the **operator** holding a snapshot of a
host that misbehaved, and the **capacity planner** asking where a
region's bytes go. What answers them now: `mise run layout --members`
(region structs, checked against `ta_system_layout`), `tinyactors inspect`
and `tinyactors actors` (a snapshot's segments, actors, mailboxes, delayed
sends), `host.stats`. The cost: nothing per event, per actor or in any
binding; a task in core, a report format of the command, a website
viewer. Out of scope: attaching to a live production host, and writing
to one (the MCP server proposal).

## Three views

- **Source.** Every type and function of an optimized build, from its
  DWARF: structs drawn to scale on a cache line with their holes, the
  layout task's costs (actor 81 B, message 106 B, delayed send 151 B),
  the call graph of the optimized code (`DW_TAG_call_site` with
  `DW_AT_call_origin`; inlined calls from `DW_TAG_inlined_subroutine`),
  laid out by layer so a call upward shows. The `debuginfo` build of
  2dc827ac has, per layer, 184 to 561 functions, 216 to 3,306 call sites
  and 318 to 1,803 inlined calls (`llvm-dwarfdump`, base, core, system,
  host).
- **Model.** A guided walk on a real host in the page (`@tinyactors/wasm`):
  compile, add an image (a segment appears), spawn, send, deploy v2,
  migrate, hibernate, send to another system, snapshot. The website's
  Architecture page explains the words today.
- **Memory.** A host's regions: each segment as a strip of its bytes by
  pool, one cell per actor slot, an actor's record decoded with every
  index a link (mailbox head to its event), the turns of the loop.

## A snapshot as the input changes the plan

The first sketch decoded a host running in the page, reading wasm linear
memory with offsets from the DWARF. With a snapshot of a real host as the
main input, five things change.

1. **A snapshot is raw memory of one build.** `host.h` ("Snapshots are
   for the same build of the library") and `docs/durability.md` say so,
   and `tinyactors inspect` reads its own format only. A snapshot from a
   binding on x86_64 cannot be decoded with the layouts of the website's
   wasm32 build, whose `chart_contexts` hold 4-byte pointers (unverified
   whether anything else differs). So **the decoder is the command of the
   same release**, not JavaScript reading private layouts:
   `tinyactors inspect --format json` already restores a snapshot without
   running datamodels and reads it with the system's inspection readers.
   It grows a report of the regions (each segment's pools: offset, bytes,
   capacity, used, high water; each actor's record fields) written with
   `offsetof` and `sizeof` of its own compile, and the viewer reads that
   report. The coupling question of the first sketch goes away: the
   viewer depends on a report format with a version, core on nothing.
2. **The snapshot must say which build wrote it.** Today the pack carries
   format numbers (`Format 3`, `Host format 4` in `inspect`), and the
   command says when it cannot read one. To tell the operator which
   release's command to run, the pack would carry the library's version
   and a hash of the region's layout, which the layout task can compute
   from DWARF. A request to core, not decided.
3. **The order of the work flips.** A snapshot viewer needs no host in
   the page and serves the operator first: drop a `.snapshot`, or the
   JSON the command wrote from it, and read the regions offline. The
   live wasm host is for teaching (the Model walk) and comes later.
   The Source view stays first: it needs neither.
4. **Time travel becomes a recording.** `tinyactors run` takes
   `run.snapshot` between turns (only on a quiescent host: no held batch,
   no migration), and writes every effect as a notification. A script of
   inputs run through it gives a snapshot per turn and the effects
   between them: the frames and the log. A Model walk recorded so is a
   static file the website shows, made by a program that ran, as the
   site's rules ask. A frame is a whole host, so recordings beyond a few
   thousand actors need frames stored as changed pages (unverified cost).
5. **What a snapshot lacks, the viewer shows as absent.** Hibernated
   actors keep their record in the region, their state is in the
   application's store; the services the application runs are not in it;
   the trace journal holds only the records not yet taken. A quickjs
   datamodel's state is opaque bytes; a binding's is a CBOR map of every
   session's variables. Those variables are the application's data, so
   the viewer runs in the reader's browser, uploads nothing, and can hide
   values and show only their shape.

## Building it

1. `mise run atlas` in tinyactors-core: the `debuginfo` build (and a
   wasm32 one), `mise/dwarf.rb` extended to read functions, call sites,
   inlined calls and line tables, written to `atlas.json` with the commit.
   Checked like `layout`: its region sizes equal `ta_system_layout`'s.
2. `tinyactors inspect --regions` (name open): the report of item 1
   above, in JSON. Checked against `tinyactors actors` of the same
   snapshot.
3. The viewer, on the website as web components beside `ta-live` and
   `ta-playground`: Source from `atlas.json`, Memory from a report or a
   recording, Model on `@tinyactors/wasm`.

## Open questions

- Does the pack carry the library's version and a layout hash (item 2)?
- Does one of the two meanings of "system" (a segment's `ta_system`; a
  host named by its system id) get another name?
- Is a recording a format of `tinyactors run` (a file of frames and
  notifications), or a script a client writes over the protocol?
