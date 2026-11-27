# ADR 0021: One MCP server for every binding

**Status:** Accepted (2026-11-27), revised the same day after a
refutation (memory stays the language's, as ADR 0020 has it; the methods'
page; typed methods, not a raw post; units; how the catalog ships; what a
second server still writes). Asked for by the maintainer: "refactor
the implementation such that it becomes really easy to ship the same MCP
server across all language ecosystems ... the main interface for
interacting with a running system."
**Date:** 2026-11-27

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: the operator, through an agent.** The MCP server is the main
  interface for a running system: inspect it, send to it, read its spans,
  snapshot it, rehearse and deploy a chart version. The operator should
  get the same tools, with the same answers, whatever language the
  application is written in.
- **Then: the binding author** who ships that server for Python now and
  for Go or Java later. Today it is 530 lines of JavaScript
  (`tinyactors-napi/mcp/index.mjs`), and each binding has its own C
  mapping of core's snapshot readers to its values (napi 631 lines, wasm
  422, cpython 537, beside the CLI's 764).
- **Out of scope:**
  - the MCP protocol itself (JSON-RPC, sessions, Streamable HTTP, auth):
    every ecosystem has an SDK for it;
  - a Python, Go or Java server: this ADR makes them small, it does not
    write them;
- **Who pays:**

  | Cost | What |
  |---|---|
  | per event, per actor | nothing: the reader works on a snapshot's bytes, never on a live host |
  | memory | a reader is one block of the caller's memory (a `Buffer`, a `bytearray`), sized by a probe: the pack's copy and each live segment's region, as ADR 0020 has it; a request's working memory comes from the caller's allocator for the call and is given back before it returns |
  | API | three host calls (`ta_host_snapshot_reader_size`, `_open`, `_request`), the `snapshot.*` methods on a page of their own (`docs/snapshot-requests.md`), and one data file, `tinyactors-core/mcp/tools.json`, which core's code never reads |
  | bindings | each binding's snapshot reader shrinks to one class over the three calls; each binding's package ships the catalog of the core it builds |

## Context

1. The MCP server's tools are of three kinds (`mcp/index.mjs`):
   - **snapshot tools** (`snapshot_summary`, `snapshot_actors`,
     `snapshot_chart`, `snapshot_traces`) read a pack's bytes and derive
     from them: an actor's leaf states, counts of actors per state, a
     chart with its counts, filters and paging, decoded spans. None of
     that needs a host or a language, and it would be written again in
     every language;
   - **host tools** (`host_stats`, `trace_watch`, `trace_unwatch`, the
     reads of `actor`) are core's requests, through each binding's typed
     methods;
   - **binding tools** need the language: the copy of a snapshot that
     `snapshot_variables` and `rehearse` restore runs the application's
     datamodel (Node's native one and Python's run in the language's
     engine), the tap wraps the application's handlers, `end_service`
     calls the binding's `Invocation`, `ask` is the binding's own
     construction, and compile, deploy, send and terminate are binding
     calls.
2. The root `AGENTS.md`: code free of any language's API belongs in a C
   layer, not in a binding. `docs/requests.md` keeps calls that need no
   host out of `ta_host_post`.
3. The control plane already has an envelope, param checks, error codes
   and a span writer (`src/host/request.c`), and every binding already
   encodes params and decodes replies for its typed methods.

## Decision

1. **Core answers requests about a snapshot, with no host.**
   `ta_host_snapshot_reader_size` says how many bytes a reader of a pack
   needs; `ta_host_snapshot_reader_open` makes one in the caller's block:
   a copy of the pack and each live segment's region read with
   `ta_system_read` (the memory is the language's, kept private, freed
   with its object: ADR 0020, decision 8). `ta_host_snapshot_reader_request`
   answers one message in the control plane's envelope (`{"id", "method",
   "params"}` in, `{"id", "result"}` or `{"id", "error"}` out), at the
   call, into the caller's `ta_buffer` (`TA_ERROR_NO_MEMORY` with the
   length it needs), its working memory from the allocator the request
   names, all given back before it returns, as `ta_host_trace_history`
   does. The methods are the `snapshot.*` area, on their own page
   (`docs/snapshot-requests.md`, which `docs/requests.md`'s "What does
   not" points to):
   - the readers, a segment at a time, as the bindings mirror them now
     (`snapshot.header`, `.stats`, `.handles`, `.actor`,
     `.configuration`, `.mailbox`, `.timers`, `.charts`, `.traceKept`,
     `.traces`);
   - what a tool asks of the whole snapshot (`snapshot.summary`,
     `snapshot.actors` with its filters and paging, `snapshot.chart` with
     the actors in each state, `snapshot.spans` decoded and filtered).
     `docs/requests.md` keeps derivations a binding can compute out of
     `ta_host_post`, because a host's methods are kept few; this reader's
     purpose is that no binding computes them.

   They are not methods of `ta_host_post`: they need no host, and a
   request names no host. The envelope, the param readers, the error
   codes and the span writer are request.c's, shared through an internal
   header: a request carries the host or the reader it answers for, and
   writes into an output its caller gives.
2. **A binding's snapshot reader is one class over the three calls.**
   `readSnapshot` and `read_snapshot` keep their methods; each is one
   request whose reply the binding decodes with the codec it has. The C
   mappings of the readers in napi, wasm and cpython go.
3. **One tool catalog, in core.** `tinyactors-core/mcp/tools.json` holds
   every tool: its name, description, input schema (JSON Schema, a small
   subset: objects of strings, numbers, integers, booleans, enums, arrays
   and records, with defaults), its annotations, the option that gates it
   (`allowDeploy`, `allowTerminate`), and what runs it:
   - `{"snapshot": "<method>"}`: the reader's method, with the tool's
     arguments but `snapshot` as params; `snapshot` names a pack the
     server keeps;
   - `{"host": {"<key>": "<method>", ...}}`: core's requests through the
     binding's typed methods, each server holding a table from a method's
     name to its typed method (no binding gains a raw post, ADR 0012,
     decision 3), their results under their keys (a refusal as
     `{"refused": message}` when the tool reads several);
   - `{"binding": "<operation>"}`: the language's own code; the catalog's
     README lists each operation and what it must do.

   An argument named `actor` is a session or the name an actor was
   spawned with, which the server resolves to `session`. A param's unit is
   its method's (`trace.watch`'s `seconds`); the binding operations' own
   are milliseconds (`timeoutMs`, `advanceMs`); in a result, an integer
   past 2^53 is its decimal text. The catalog is data for the servers, not
   a decision of core's: no code of core reads it. It lives beside core
   because every binding builds the core of its release, and each
   binding's package ships that core's copy (Node: `@tinyactors/node`
   exports it; Python's module embeds it, `tinyactors.mcp_tools()`), so a server and its
   binding's methods come from one release. A change to it reruns every
   binding's tests (their `#MISE sources`).
4. **A server per language is the catalog, its SDK, and the binding
   operations.** It lists the catalog's tools, checks arguments against
   their schemas, forwards `snapshot` and `host` tools, and implements
   the binding operations: the tap, a copy restored with stand-ins
   (`snapshot_variables`, `rehearse`), compile and deploy, send, ask and
   terminate, the services, the tap's spans and logs, and its transport.
   That is most of the lines a second server writes, and none of it can
   leave the language; what leaves is everything about reading a
   snapshot, the tools' names, schemas and descriptions, and their
   rules. Node's is first, Python's second (2026-11-27, the maintainer:
   "build out the python version").
5. **The MCP suite holds them to one behaviour** (2026-11-27, the
   maintainer: "extend the compliance suite to make sure all MCP server
   implementations behave the same"): `tinyactors-test/suites/mcp`, run
   against each server's fixture program (`tinyactors-test/docs/mcp.md`),
   which speaks MCP's stdio transport plus four `fixture/*` methods (open
   a host of the fixture, settle, advance its simulated clock, spawn). A
   case calls the tools as an agent would and matches each result's
   structured content against what it expects: what every server must
   give alike, never what a language gives by design.

## Rejected

- **The MCP protocol in C** (`src/cli/rpc.c` reads JSON-RPC lines): MCP
  moves fast (protocol versions, sessions, Streamable HTTP, auth), each
  ecosystem's SDK keeps up with it, and core would have to own JSON and
  transports.
- **One server outside the application**: a copy of a snapshot runs the
  application's datamodel, the tap wraps its handlers, and no binding
  offers a raw post (ADR 0012, decision 3). Only the snapshot tools could
  leave the process, and they now cost a binding nothing.
- **The snapshot methods on `ta_host_post`**: they need no host, and a
  host would have to keep a pack it has no use for.
