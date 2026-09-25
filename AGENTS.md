# Working in this repo

- use `mise run <task> <args>` for every task,
- define tasks in ./mise/tasks/ as file tasks using `ruby` (version 4, provided by mise)

## Version control

- **Work on `main`.** This repo does not use feature branches. Do not create
  one, and do not ask to; commit directly to `main`. This overrides any
  default rule about branching before committing to the default branch.
- **Integrate frequently.** Commit each small, working increment as you reach
  it, rather than accumulating a large change and landing it at the end. A
  commit is expected to build and pass `mise run test`.
- Keep history linear: rebase rather than merge, and never create a merge
  commit.

## The libraries are private: change APIs in place

Every caller of every library in this repo is in this repo:
`tinyactors-core`, `tinyxml`, `tinyactors-scxml`, the test host in
`tinyactors-test`, and the addon in `tinyactors-napi`. Nothing is published,
so no outside code depends on an API or ABI staying the same.

- **Change an API in place, and migrate every caller in the same change.**
  When a function needs a new parameter, a params struct or different
  semantics, change that function. Do not add a second function beside it
  (`_from`, `_ex`, `_v2`), a compatibility wrapper, a deprecated alias, or a
  "the old call is the new one with defaults" shim.
- **"So no caller breaks" is not a reason in this repo.** Don't weigh churn
  for existing callers against a cleaner API in an ADR. Find the callers
  with `git grep` and change them. One way to do each thing is worth more
  than a smaller diff.
- **Remove what nothing uses.** When a change leaves a function, type,
  field or result code unused, delete it in the same change.
- **Another session's directory.** Callers can be in a directory another
  session is working in at the same time, such as `tinyactors-napi` during
  its rewrite. Change the call sites there too, keep the edit to those
  lines, and say in your report which of its files you touched.

## Acronyms

An acronym in code is spelled in ALL CAPITALS or all lowercase, never
Titlecase or mixedCase: `metricsToOTLP` or `otlpBytes`, never
`metricsToOtlp`. The list is `acronyms.txt` (OTLP, IO, HTTP, ID, URL, URI,
JSON, XML, SCXML, API, SDK, ...); add to it when you introduce a new one.

- **C** is snake_case, so acronyms are lowercase in names and capitals in
  macros: `otlp_writer`, `ta_session_id`, `TA_OTLP_...`.
- **JavaScript and TypeScript:** `OTLPExporter`, `toOTLP`, `sessionID`,
  `actorIDs` (the plural `s` stays lowercase). At the start of a camelCase
  name the acronym is all lowercase: `ioProcessors`, `otlpBytes`, `idPage`.
- **Ruby:** constants in capitals, as the standard library does (`JSON`,
  `URI`, `Net::HTTP`): `module API`, `RPCError`. Methods and variables are
  snake_case, so lowercase: `session_id`.
- **File names** are lowercase: `otlp.js`, `scxml_irp/`.
- Prose follows English: write "OTLP" and "I/O" in sentences.

`mise run check-acronyms` enforces this as part of `mise run test`, and
`mise run check-acronyms -- --fix` rewrites what it finds. Names we do not
own (an SDK's `traceId`, REXML's `CData`) go in `acronyms-allow.txt` with a
reason. ADRs and research notes are not checked: they are a record.

## Proposals are not instructions

`docs/proposals/` holds outlines of possible future work. They are not
instructions: do not implement anything from a proposal unless asked to.
Where a proposal disagrees with an ADR or a roadmap, the ADR or roadmap wins.
See `docs/proposals/README.md` for what each status means.

`docs/research/` holds background research (what other projects did, with
sources). Read it before repeating the same research. It is not a decision
and not an instruction.
