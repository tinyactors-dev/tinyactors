# The tinyactors command

`tinyactors` compiles charts and system manifests into images, and looks
into charts, manifests and images: what they hold, how their executable
content runs, how two versions differ. Every command answers in Markdown
for people, or in JSON or XML for programs, with the same members in the
same order.

It serves the application developer first (why does this chart do that,
what changes if I deploy this version), then the operator and the C
embedder, who get an image they did not compile and want to know what it
is, and the maintainers, who read the compiler's output instruction by
instruction. It costs nothing at run time: it is a program beside the
libraries, built from them, and in none of them.

## Synopsis

```text
tinyactors [options] <command> [options] [arguments]
tinyactors help [<command>]
```

Commands take their options anywhere after the command, and the program's
options (below) anywhere on the line. Options follow GNU's conventions:
`--name VALUE` or `--name=VALUE`, `-n VALUE` or `-nVALUE`, flags clustered
(`-Wf json`), `--` before arguments that start with `-`, and `-` for standard
input. A long name is matched whole, never by a prefix. A mistyped command
or option is refused with the nearest one ("did you mean 'inspect'?").

## Inputs

A command that looks into an image takes, as its arguments:

- one compiled image (`.tai`), read as it is and verified;
- one system manifest (a `<system>` root): its charts and boot actors,
  compiled;
- or SCXML charts (`<scxml>` roots), compiled together into one image, the
  first the default boot chart.

The kind is read from the bytes, not the file's name: an image starts with
`TAI1`, and XML is told by its root element. A chart's static `src` is read
relative to the file that names it. `-` reads standard input. Compiled
inputs give their diagnostics with the answer.

## Output

`-f` / `--format` chooses `markdown` (the default), `json` or `xml`. The
answer goes to standard output; messages about the command line or a file
that cannot be read go to standard error as `tinyactors: <message>`.

Every answer is one document with the command's name as its title:

- **Markdown**: the title as a heading; an object's values as
  `- **Name**: value` lines; each list of flat records as a table; nested
  objects under headings; a diagram as a fenced block (Mermaid for
  `graph`).
- **JSON**: one object, members in order, two spaces of indent. Numbers
  are integers, text is UTF-8.
- **XML**: the title as the root element; an object's values as its
  attributes, its lists and objects as child elements; a list's items as
  elements named for the item (`<chart>`, `<state>`, ...).

Member names are camelCase (`bootActors`, `doneEvent`) and are the same in
all three formats: a script reads `charts[0].states` in JSON and
`/states/charts/chart/states/state` in XML. A state is named by its id;
the root (the `<scxml>` element itself) by `<scxml>`, which no id can be.

## Commands

| Command | Arguments | What it answers |
|---|---|---|
| `compile` | INPUT... | Compiles charts or one manifest into an image written to `-o FILE` (required; `-` for standard output, the answer then going to standard error), as the image's bytes or, with `--emit c`, an 8-aligned C array named by `--symbol` for ROM or flash. The answer: whether it compiled, the image's size, hash, charts and boot actors, and the diagnostics. A file is written whole or not at all. |
| `check` | INPUT... | Compiles the inputs or verifies the image, writing nothing: valid or not, and the diagnostics. |
| `inspect` | INPUT... | The image: its size, format version, hash, sections (offset, bytes, records), charts (source, datamodel, binding, counts of states, transitions, data, expressions, sends, invokes and code), boot actors. |
| `states` | INPUT... | Each chart's states in document order: id, kind, parent, depth, initial targets, counts of transitions, invokes and data, whether it has onentry and onexit, its done event. |
| `transitions` | INPUT... | Each chart's transitions: kind (transition, initial, history default), source, event descriptors, condition, targets, type, whether it has actions. |
| `events` | INPUT... | Every event name of the image, and how many transitions handle it and raises and sends make it by name. |
| `data` | INPUT... | Each chart's `<data>`: id, the state declaring it, its expression and line. |
| `expressions` | INPUT... | Each chart's expressions and scripts: kind, line, column, text; `--kind` keeps some (`condition`, `value`, `location`, `script`, `string`, `content`, `item`, `global-script`). |
| `disassemble` | INPUT... | Each chart's executable content as instructions: their place, the entry points starting there (onentry and onexit of a state, a transition's actions, an invoke's finalize, the chart's script), the instruction and its operands. |
| `graph` | INPUT... | Each chart as nodes (states) and edges (one per target of each transition), and as a Mermaid state diagram. |
| `diff` | OLD NEW | Two versions, each an image, a manifest or a chart, compared chart by chart (matched by name): states, data, transitions and events added and removed, and what a migration does with an actor in a state the new version removed. `--exit-code` exits with 1 when they differ. |
| `help` | [COMMAND] | The program's commands, or a command's usage, options and examples. |
| `version` | | The program's version, its library's ABI and the image format version it reads. |

The commands that take inputs also take `-W` / `--warnings-as-errors` (a
compiled input with a warning is refused), and all but `compile` and
`check` take `-c` / `--chart NAME`, repeatable, to look at those charts
only.

Program options: `-f` / `--format FORMAT`, `-h` / `--help` (any command:
describe it), `--version`.

## Exit status

| Status | When |
|---|---|
| 0 | the command did what was asked |
| 1 | an input was refused: a chart did not compile, an image did not verify, a file is none of the three; or `diff --exit-code` found differences |
| 2 | the command line was not understood (and `tinyactors` alone, after printing the commands) |
| 3 | a file could not be read or written, or memory ran out |

## Examples

```sh
tinyactors compile order.scxml payment.scxml -o order.tai
tinyactors check -W charts/*.scxml
tinyactors inspect --format json order.tai | jq '.charts[].name'
tinyactors transitions --chart order order.scxml
tinyactors graph order.scxml > order.md        # renders as a diagram on GitHub
tinyactors diff --exit-code deployed.tai order.scxml
tinyactors compile order.scxml -o - | tinyactors disassemble -
```

## How it is built

Three layers, from the bottom:

- **`src/cli`**, freestanding like `src/base`: `arguments.h` reads a
  command line against a program's description (its commands, their
  options and arguments), and `report.h` builds an answer as one tree and
  writes it in the three formats. Neither uses the C library, recurses or
  allocates but from the arena it is given.
- **`tools/tinyactors`** but one file: the commands. They compile without
  the C library's headers and reach files only through the platform
  interface in `tool.h`. One invocation is one arena: the parsed line, the
  files read, the compiled image and the answer live in it, and it is freed
  once when the command ends.
- **`tools/tinyactors/platform.c`**: `main`, the allocator behind the
  arena, reading files and standard input, writing streams, and writing a
  file whole (a temporary file renamed over it). The only file that uses
  the C library.

The program is built with core (`mise run build`, `build/dev/bin/tinyactors`)
and tested by running it (`tests/unit/test_tool.c`). It is not in the
release assets yet.
