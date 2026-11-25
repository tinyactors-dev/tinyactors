# tinyactors-core 0.4.0

### Summary

This release is about the `tinyactors` command (9f2a464). It is one program, with git-style subcommands, for compiled images, system manifests and SCXML charts. It compiles and checks charts and lists what is in them, disassembles their executable content (f0007bc), draws them as SVG (89ab273), lints them and finds the shortest path to a state, and formats them in one canonical layout. It compares two versions and says what a migration does to running actors (4703f44), and it reads snapshots and blobs. Each release now carries it for macOS and Linux, x86_64 and aarch64, static on Linux, installable with mise from the release repository (9cbcfba). `tinyactors run` serves one host with quickjs over JSON-RPC 2.0 on standard input and output: spawns, sends, turns, held batches, services, hibernation, migration, snapshots and telemetry, with its protocol as a JSON Schema. Core's conformance suite runs against it. With QuickJS compiled in, the command is about 2 MB.

Snapshots now carry a description of their memory layout, so a reader can decode one without the build that wrote it, and a restore refuses a snapshot of another layout (7f4ecf9, 22daf06). A snapshot also keeps the most recent traces and describes the chart image and the instances, so a reader can show an actor's chart and its traces up to the time of capture (4255811). The host counts the bytes it allocates, in `ta_host_stats` and in the OTLP metrics. The ECMAScript datamodel can take a seed for `Math.random` and read the host's clock, so a run can repeat exactly (22815fe).

Some behaviour changes under existing code. The compiler refuses markup that SCXML does not allow, which it used to skip (4059c44). quickjs stops a busy evaluation sooner (a180547). Session ids wrap around their segment's range, so a long-running host no longer hands out ids that belong to another segment (b4ae809). Data sent between charts of two datamodels is now converted for the receiver, and a send that waits for memory no longer evaluates its expressions twice. Snapshots of earlier builds are refused.

### Upgrading

- `ta_host_time` is removed. `ta_host_advance_time` takes a `ta_system_time` with the same two fields: rename the type (22815fe).
- `ta_system_advance_time` takes a `ta_system_time` instead of a `ta_time`: pass the monotonic time and `wall_ns` (0 when the wall clock is unknown) (22815fe).
- `ta_datamodel_ecmascript_config` gains `random_seed` and `clock`. A positional initializer must add them; a zeroed config behaves as before (22815fe).
- `ta_host_snapshot_contents` gains `layout` and `layout_id`. Code that initializes it by position must add them (7f4ecf9).
- `ta_host_stats` gains `allocated_bytes`, `allocated_peak` and `allocations`, and `ta_system_config` gains `last_serial` (0: the range ends at UINT64_MAX). Both structs are larger, so recompile code that uses them.
- `ta_host_spawn` and `ta_system_spawn` can return `TA_ERROR_LIMIT` when every id of a segment's range is held, and in a system a migration exported a session from, once its range has run out; an `<invoke>` then gets `error.execution` (b4ae809). A session id can name a new session once its range has been used up, so do not keep an address after its session ends and expect it to stay unused. `ta_system_stats` spawned now counts spawns instead of reading the last id, and `ta_system_set_next_serial` takes any id of the range.
- Snapshots of earlier builds are refused (b4ae809, 7f4ecf9, 22daf06, 4255811). `TA_SYSTEM_VERSION` is removed: the region's layout id replaces it. Take a new snapshot with this build. traceBytes now also bounds the trace history a snapshot holds (4255811).
- A chart with markup that SCXML does not allow now fails to compile. This includes an unknown or misplaced SCXML element, contradictory attributes, an attribute and content for one value, a duplicated `<data id>` or `<invoke id>`, a history type other than deep or shallow, a version other than 1.0 and an `<scxml>` root in another namespace. Fix the markup the diagnostic names. An attribute that SCXML does not define is a warning, and an error with `TA_COMPILE_WERROR` (4059c44).
- The default `max_ops` of quickjs is now 10,000 checks, not 1,000,000 (`TA_DATAMODEL_ECMASCRIPT_MAX_OPS_DEFAULT`). This is about 10^8 operations. A longer single evaluation gets `error.execution` with reason "fuel": set `max_ops` to what the work needs (a180547).
- A value sent between charts of two datamodels now goes through the sender's to_wire and the receiver's from_wire. A datamodel without from_wire receives no data, and a value the sender cannot write gives no data and `error.execution` (`error.communication` across segments) (0adae4e).
- The command's own breaking changes (9f2a464, 89ab273, 4703f44) touch nothing an earlier release shipped: ta-compile, graph and diff were all added after 0.3.0. If you built ta-compile from source, run `tinyactors compile` with the same arguments (`--emit-c NAME` is `--emit c --symbol NAME`, `--dump` is `inspect`).

### Breaking changes

- a snapshot keeps the trace history and describes the chart image and the instances (4255811)
- a snapshot's description resolves its strings, and unpack checks its id (22daf06)
- snapshots describe their memory layout, and a restore refuses another (7f4ecf9)
- a seed and the host's clock for the ECMAScript datamodel (22815fe)
- tinyactors graph draws charts as SVG with scxmljs's layout, ported; CLI rough edges fixed (89ab273)
- tinyactors diff compares two versions as statecharts and says what breaks running actors (4703f44)
- the tinyactors command: git-style commands over charts, manifests and images (9f2a464)
- session ids wrap around their segment's range, skipping the sessions it holds (b4ae809)
- the compiler refuses what SCXML does not allow instead of skipping it (4059c44)
- quickjs stops a busy evaluation after 10,000 checks, not 1,000,000 (a180547)

### API changes

**Removed**

- struct `ta_host_time`

**Added**

- constant `TA_ERROR_LIMIT`
- function `ta_datamodel_environment_time`
- function `ta_library_version`
- function `ta_system_layout_description`
- struct `ta_datamodel_time`
- struct `ta_system_time`
- struct `ta_version`
- constant `TA_DATAMODEL_ECMASCRIPT_CLOCK_ENGINE`
- constant `TA_DATAMODEL_ECMASCRIPT_CLOCK_HOST`
- constant `TA_DATAMODEL_ECMASCRIPT_MAX_OPS_DEFAULT`
- enum `ta_datamodel_ecmascript_clock`
- function `ta_datamodel_ecmascript_heap_blocks`
- function `ta_xml_writer_attribute_markup`
- function `ta_xml_writer_blank_line`
- function `ta_xml_writer_comment`
- function `ta_xml_writer_instruction`
- function `ta_xml_writer_markup`

**Changed**

- function `ta_host_advance_time`
- function `ta_system_advance_time`
- struct `ta_host_snapshot_contents`
- struct `ta_host_stats`
- struct `ta_system_config`
- struct `ta_datamodel_ecmascript_config`

### Changes

**Added**

- mise run atlas, the layers' types, functions and calls from their debug information (0a1f4aa)
- quickjs counts its blocks (e0f7d1a)
- the host's metrics export the bytes it allocates (37e6133)
- host.stats counts the bytes the host allocates (420f186)
- tinyactors run writes telemetry, takes run.post and ends traces (23c91c8)
- tinyactors run snapshots and restores its host (2dc827a)
- tinyactors run hibernates, wakes, sweeps and migrates actors (3a4bb15)
- tinyactors run holds batches for the client and runs its services (1c7e060)
- tinyactors run, one host served over JSON-RPC 2.0 on standard input and output (9039019)
- JSON-RPC 2.0 lines and envelopes for the run server (32d91e9)
- the diagram check finds labels astray and edges that overlap (da1fa44)
- JSON text to CBOR and back for the command, its numbers read by a port of Go's strconv (ae191c2)
- the run protocol's JSON Schema, generated into tables a checker walks (ecdc1c3)
- tinyactors fmt keeps one blank line where the source has one or more between elements (248e64a)
- tinyactors graph lays charts out top to bottom, with themes, highlights, paths and folding (6c4778c)
- tinyactors reads snapshots and blobs: inspect them, and list a snapshot's actors (e392fcd)
- tinyactors fmt writes charts and manifests in one canonical layout, and -I DIR adds an include path for src (c1b8e86)
- tinyactors lint and path: static analysis of charts and the shortest way to a state (779ce86)
- every release carries the tinyactors command, installable with mise (9cbcfba)
- tinyactors disassemble, graph and diff (f0007bc)
- a freestanding command-line parser and a report written as Markdown, JSON or XML (01bafed)
- ta-compile, the compiler's command line (3a24e95)
- ta_library_version, the version a program runs with (b336598)
- mise run layout answers per actor, per message and per delayed send (6e771a7)
- mise run layout reads the region's types from debug information (fb3e842)

**Fixed**

- compile's help example compiles a manifest as an input (2ab09c1)
- the memory examples' hibernated row holds hibernated actors only (120604b)
- tinyactors run gives core a systemID that is not text (8ff6b6f)
- the run protocol's schema lists host.stats' allocation counts (fb65117)
- diagram edges keep their own lines and labels stay near them (a6b4934)
- graph routes an edge between parallel regions beside them, never through a box (62b6ce0)
- graph's edges meet lone ports at their middle and run into finals head on (9106bb7)
- graph's labels keep off other edges' lines and inside their edge's container (687eb01)
- tinyactors help names svg among --format's values, and data's summary reads as words (836486e)
- graph's SVG keeps to itself inline, its arrows touch their boxes, and ta_diagram_check finds what a drawing must not show (e635254)
- a send, cancel or invoke that waits for the slab evaluates nothing twice (25ee9c8)
- data that crosses between charts of two datamodels is converted (0adae4e)
- ta_image_open refuses a word that is not an opcode in its fallback decode (b2b4ee8)
- test_otlp's fixed request follows the version (066048e)

**Changed**

- tool_image_make, the image of the inputs in an arena the caller gives (2c51615)
- ta_format_double and ta_format_quoted in base/format (843c1f8)

<details>
<summary>Internal (6)</summary>

- docs/cli.md names exactly the commands and options of tinyactors (445766a)
- the conformance suite against tinyactors run (af3b9f3)
- the layout check runs in the fast pass (c537c57)
- a writing-c skill holds the C rules, and AGENTS.md asks for it (527c951)
- describe what is there: the image format, both bindings, blob versions (3f2955f)
- an actor costs 81 B of region, as mise run layout measures (2d959d1)

</details>
