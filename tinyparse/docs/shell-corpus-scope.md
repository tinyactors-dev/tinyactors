# std.shell corpus scope: what we import, and what we exclude

`std.shell` parses the **POSIX sh** command language. Its conformance corpus
(`test/suite/shellfamily/`) is seeded from the build-time tree-sitter-bash
reference (`reference/tree-sitter-bash/test/corpus/{commands,statements,literals,programs}.txt`),
but tree-sitter-bash describes **bash**, a superset. We import only the sections
whose inputs are POSIX shell *and* which std.shell parses cleanly (exit 0, no
`ERROR`/`MISSING` node); everything else is excluded on purpose.

The corpus extraction takes the input half of each `===`-delimited section (the
text before the `---` separator), classifies it, and — for the POSIX ones — writes
it verbatim to `test/suite/shellfamily/inputs/<file>__N.sh` with a matching
`<file>__N.test`. The two reasons a section is excluded are below.

## 1. Bash-only constructs (out of POSIX scope)

These are bash/ksh extensions with no POSIX sh meaning. A POSIX shell either
errors on them or reads them differently, so std.shell does not implement them and
their corpus sections are not imported.

| Construct | Example | Corpus section(s) |
|---|---|---|
| Arrays and array subscripts | `a=(1 2 3)`, `${a[i]}`, `a+=(x)` | literals: Arrays and array expansions; Subscripts |
| `[[ … ]]` conditional command | `[[ $x == y ]]` | statements: Test commands, Test commands with regexes, …; If statements with conditional expressions |
| `(( … ))` arithmetic command | `(( i++ ))`, `((foo=0))` | statements: Test commands with ternary; literals: `((foo=0))` line |
| C-style `for ((;;))` | `for (( i=0; i<5; i++ ))` | statements: C-style for statements |
| `select` loops | `select x in …; do … done` | statements: Select statements |
| `$'…'` / `$"…"` (ANSI-C / locale strings) | `$'a\n'` | literals: Strings with ANSI-C quoting |
| Process substitution | `<(cmd)`, `>(cmd)` | literals: Process substitutions; statements: While with `< <(…)` |
| Here-strings | `cmd <<< word` | commands: Herestrings |
| Brace expansion | `{a,b}`, `{0..5}` | literals: Brace expressions and lookalikes |
| `function` keyword form | `function f { … }` | the `function …` cases in Function definitions |
| `time` as a pipeline keyword | `time ( … )` | the `time ( … )` case in Subshells |
| Case fall-through `;&` / `;;&` | `a) … ;&` | the `;&` cases in Case statements |
| `&>` / `>&word` combined redirect, `|&` | `&>/dev/null` | the `&>` case in File redirects |
| `declare` / `typeset` / `local` / `export` / `readonly` / `unset` **as syntax** | `declare -i x=1` | statements: Variable declaration: declare & typeset / readonly / local / export; Unset commands |
| Substring / case-mod / indirect / transform expansions | `${v:off:len}`, `${v^^}`, `${v,,}`, `${!v}`, `${!pre*}`, `${v@U}`, `${v/a/b}` | literals: Variable Expansions: Length / with operators / Bizarre / Weird / Regex |
| `$[ … ]` legacy arithmetic | `$[a + 1]` | the `$[…]` line in Arithmetic expansions |

Notes:

- `declare`/`local`/`export`/etc. still parse as **ordinary simple commands**
  (`export` is just a command word and `PATH=x` an assignment-prefixed argument);
  what we exclude is bash's *declaration-command* grammar that gives them special
  assignment parsing. We do not import the reference sections that depend on that
  special parsing.
- `[ … ]` (the `test` builtin) IS imported — `[`, `=`, `!=`, `]` are ordinary
  words, so `[ "$a" = b ]` is a plain simple command and parses. Only the `[[ … ]]`
  keyword form is bash-only.
- The reference sometimes mixes a valid POSIX line with a bash-only one in a single
  section; in that case we import the POSIX line(s) and drop the bash-only one
  (e.g. `loop=; variables=& here=` keeps the assignments but drops a trailing `;;`
  that is a syntax error outside `case`).

## 2. Known std.shell limitations (from earlier adversarial reviews)

These are POSIX constructs std.shell does not yet parse correctly. Inputs that
depend on them are not imported. They are tracked as future work.

| Limitation | Example | Why excluded |
|---|---|---|
| Quoted here-document delimiters | `<<'EOF'`, `<<"EOF"`, `<<\EOF` | the captured delimiter keeps its quotes, so the body's closing line never matches |
| `$12` etc. | `$12` | parsed as positional parameter 12 (`${1}2` is the POSIX reading) |
| Arithmetic sub-groups that touch `(…)+(…)` | `$((a)+(b))` | the balanced-text arithmetic body mis-handles adjacent groups |
| Closing here-document delimiter on a final line with no trailing newline | `cat <<E…E` (EOF, no `\n`) | the delimiter line needs its terminating newline to match |

## Bug fixed while importing this corpus (S8)

Importing the literals corpus surfaced one real grammar bug, now fixed: a literal
`$` inside a double-quoted string (`echo "s$"`, `echo "s/$/x/"`) had no rule to
match it — `DQText` excludes `$`, `Expansion` fails when no name follows — so the
string parse failed. Added a `DQDollar` fallback token to the `DoubleQuoted` body,
mirroring the unquoted `DollarLiteral`: a real `$name`/`${…}`/`$(…)`/`$((…))` is
still claimed by `Expansion` first; only a stranded `$` falls through.
