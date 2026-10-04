---
name: add-std-grammar
description: Add a new grammar (or grammar family) to the tinyparse standard library under `grammars/std/`. Use when porting a language to tinyparse as a built-in, extending an existing std grammar with a dialect, or establishing the full lifecycle — reference pin, package manifest, grammar source, test corpus, xfail seed, build embedding — for a grammar that will live alongside std.javascript and friends.
---

# Adding a standard grammar to tinyparse

The JS/TS family is the canonical template: `std.javascript` is the base;
`std.typescript` and `std.tsx` extend it with `(extends std.javascript)` and
`(override …)` the rules their type system touches. Follow that shape for every
new addition. Work directly on `main`; one commit per meaningful step.

---

## Step 1 — Pin the reference grammar in `reference/PINS.txt`

A tree-sitter grammar (or equivalent) is a **build-time porting reference only**:
never linked, never shipped, gitignored. Its `test/corpus/` is the source for our
conformance tests; its grammar source is what you study, not what you copy.

Add one line per reference to `reference/PINS.txt`:

```
tree-sitter-sql  https://github.com/DerekStride/tree-sitter-sql  <full-40-char-commit>
```

Include the re-fetch commands as comments (see existing entries for the format).
Pin the exact commit you cloned and extracted corpus from — not `HEAD`, not a tag.

**Failure modes**

- *Wrong commit pinned.* You cloned at HEAD, corpus was extracted, then you wrote
  down a different SHA. The reference and the corpus diverge silently. Fix: pin
  the commit you actually extracted, verified with `git -C reference/<name> rev-parse HEAD`.
  In the web environment `git rev-parse` reads the host repo instead of the
  reference tarball — trust `PINS.txt` and set the SHA manually from the tarball
  URL you used.
- *Clone fails in the web environment.* Outbound `git clone` is rewritten to a
  proxy that returns 403 for repos outside this one. Fetch as a tarball instead:
  ```sh
  curl -sSL --cacert /root/.ccr/ca-bundle.crt \
    -o ref.tar.gz https://codeload.github.com/<owner>/<repo>/tar.gz/<commit>
  tar -xf ref.tar.gz -C reference/
  ```
  The extracted directory will be `<repo>-<short-sha>/`; rename it to match what
  `PINS.txt` re-fetch commands expect.

---

## Step 2 — Create the package directory structure

For a single grammar:

```
grammars/std/<name>/
    package.scm
    grammar.scm
```

For a family (base + dialects):

```
grammars/std/<base>/
    package.scm
    grammar.scm
grammars/std/<dialect1>/
    package.scm
    grammar.scm
```

`mise run generate` (which `mise run build` runs) scans `grammars/std/*/package.scm`
automatically — no build file to edit.

**Failure modes**

- *Directory name does not match package name.* The CLI resolver uses the `name:`
  field in `package.scm`, not the directory name, to find a grammar. Mismatches
  produce confusing "grammar not found" errors. Keep them identical.
- *Missing a file.* `gen_grammars` expects both `package.scm` and `grammar.scm`
  to be present. A directory with only one will embed an empty or broken entry.
  Create both before running the build.

---

## Step 3 — Write `package.scm`

The manifest declares name, version, parameters, optional `extends:`, and the
entry rule the host calls as `provides:`.

**Base dialect:**

```scheme
; package.scm — std.<name>
; One sentence: what language this covers and where it was ported from.
; Ported (the LANGUAGE, not the code) from the build-time reference under
; tools/tinyparse/reference/<reference-name>/. No third-party code is copied.

(tinyparse version: "0.1")

(package
  name: std.<name>
  version: "0.1.0"
  params: (<param1>: #f <param2>: #f)
  provides: (program))
```

**Dialect that extends the base:**

```scheme
(tinyparse version: "0.1")

(package
  name: std.<dialect>
  version: "0.1.0"
  extends: (std.<name>)
  params: (<param1>: #t <param2>: #f)
  provides: (program))
```

Params listed in `params:` are the knobs the grammar source can gate rules on
with `(when <param>: …)`. The `extends:` value is passed through to the compiler
with the params overridden.

**Failure modes**

- *`provides:` rule name does not match `start:` in `grammar.scm`.* The entry
  point mismatch is a compile-time error from the grammar loader. They must be
  identical symbols.
- *`extends:` names a package not in the embedded registry.* Unresolved extends
  fail at grammar-compile time with a "package not found" error. The full chain
  must be reachable — either all packages are in `grammars/std/` (and therefore
  embedded by `mise run generate`) or the path is resolved by the grammar compiler from
  file paths. For a new family, add all members to `grammars/std/` before testing
  any extending dialect.
- *`params:` omits a param used in `grammar.scm`.* A `(when missing-param: …)`
  in the grammar source will fail at compile time or silently never fire. Declare
  every param the grammar body references.

---

## Step 4 — Write `grammar.scm` — the base grammar

Start the file with the module declaration and grammar head:

```scheme
; grammar.scm — std.<name>
;
; <Short description of what the language is and what hard problems the grammar
; solves — regex-vs-division, ASI, case-insensitivity, whatever is non-obvious
; for this language.>
;
; Ported (the LANGUAGE, not the code) from reference/<...>. Node names follow
; tree-sitter where reasonable.

(tinyparse version: "0.1")

(module std.<name>)

(grammar <name>
  start: program

  ; --- trivia
  (token Space (repeat (choice (regex "[ \t\r\n]") …) min: 1))
  (token LineComment (seq "--" (repeat (not "\n" any))))
  (token BlockComment (seq "/*" … "*/"))

  (extras
    (trivia Space)
    (trivia LineComment  attach: leading)
    (trivia BlockComment attach: leading))

  ; --- tokens: keywords, literals, operators, identifiers
  ; --- rules: top-down from program down to primary expressions
)
```

**Naming convention: CamelCase throughout, even when porting.**  Tokens and rules
both use CamelCase (`Space`, `LineComment`, `BlockComment`, `SelectKeyword`,
`BinaryExpression`). Do not adopt the reference grammar's naming when it is
snake_case or uses a different convention — rename to CamelCase at the porting
boundary. Consistency within the package matters more than matching the source.

Key structural decisions to make before writing rules:

- **Expression precedence ladder.** Write from lowest to highest: `or > and > not
  > comparison > additive > multiplicative > unary > postfix > primary`. Each level
  is `(rule level_n (seq level_n+1 (optional (seq op level_n+1))))` (left-factored
  from the start). See `docs/grammar.md` §Hidden rules for the `(hidden …)` wrapper.
- **Reserved words.** Any keyword that must not appear as a bare identifier needs
  `(reserved KEYWORD_TOKEN)` to suppress it from the identifier rule. List them
  exhaustively — a missed keyword becomes a parse ambiguity.
- **Left-recursion is illegal in PEG.** Express `A ::= A op B | B` as
  `(rule A (seq B (optional (seq op B))))`. This is left-factored by definition.
  See the left-factor skill for the full treatment.

**Failure modes**

- *Duplicate token or rule name.* The grammar compiler rejects duplicates. Read the
  error: it names both definition sites.
- *Left-recursion.* A rule that references itself directly or through a chain will
  loop forever. The symptom is an immediate hang on any input. Rewrite using the
  `(seq base (repeat (seq op base)))` pattern.
- *Case-insensitive keywords matching identifiers.* SQL, COBOL, etc. have
  case-insensitive keywords. If `(token SELECT (regex "[Ss][Ee][Ll][Ee][Cc][Tt]"))`
  is listed after the generic `identifier` token in an ordered choice, it never
  wins. Put keyword tokens before the identifier token in every `(choice …)` where
  both could match, and use `(reserved SELECT)` to block `select` from matching
  as an identifier when SELECT is in scope.
- *Regex character class for high bytes.* The engine's regex subset has no Unicode
  property escapes. To match any UTF-8 continuation or lead byte (for unquoted
  identifiers in scripts that allow them), use `(regex "\\x80-\\xFF")` — the `\xHH`
  escape is the engine's extension; a raw high byte in the string is not portable
  across readers.
- *`(not X any)` semantics.* `(not X any)` matches one byte that does not match X,
  not zero bytes. Using it in a `(repeat …)` to match "everything up to a
  delimiter" is correct; using it to mean "optionally consume if not X" is wrong.
- *Extras firing inside a token.* Extras fire between grammar elements, never inside
  a `(token …)` body. A SQL string literal `'hello world'` does not consume a Space
  extra in the middle — the token rule must include the space itself. Only inter-token
  whitespace is handled by extras.

---

## Step 5 — Write dialect grammar extensions

A dialect grammar adds and overrides; it does not re-state the base.

```scheme
(tinyparse version: "0.1")

(module std.<dialect>)

(extends std.<base>)

; Add new tokens the dialect introduces.
(token NewOperator "::")

; Override a base rule to thread in the new forms.
(override (rule expression
  (choice
    dialect_specific_expression
    base_expression_forms…)))

; Gate a production on a param.
(rule something
  (seq
    (when some_param: special_form)
    base_form))
```

`(override (rule …))` replaces the named rule entirely in the merged grammar.
`(extends …)` at the top of the file makes all base tokens and rules available
without re-declaring them.

**Failure modes**

- *Override names a rule that does not exist in the base.* The compiler treats it
  as a new rule definition, not a replacement. The base rule survives alongside the
  override, causing ambiguity or the wrong rule being used as the entry. Double-check
  the base rule name before writing `(override …)`.
- *Override loses field names from the base rule.* When replacing a rule that bound
  fields (`left:`, `right:`), the override must re-declare the same field names if
  callers depend on them — tree queries break silently if a field disappears.
- *`(when param: …)` on a param not declared in `package.scm`.* The form either
  errors at compile time or is silently skipped. Declare every param the grammar
  body references (step 3).
- *Dialect extends base that extends another base.* Chains work, but the middle
  package must be fully embedded (step 2). A `std.postgres` that extends `std.sql`
  which is not yet embedded produces "package not found" for `std.sql`, not
  for `std.postgres` — read the full error chain.

---

## Step 6 — Import the reference corpus as test inputs

The reference grammar's `test/corpus/*.txt` (tree-sitter format) contains
alternating input / expected-output pairs. We import the **input halves only**.

For each corpus section extract the input block and write it as an individual
file under `test/suite/<family>inputs/`:

```sh
# Rough extraction script — adjust field separator for your reference format.
# tree-sitter corpus: sections separated by "===…" headers, input before "---" separator.
```

Name the files descriptively after their corpus section header:
`expressions__1.sql`, `declarations__2.sql`, etc. (double underscore matches
the existing jsfamily and tsfamily convention).

**Failure modes**

- *Importing expected-tree output alongside the input.* The test assertions here are
  `out-absent ERROR` and `out-absent MISSING`, not tree-equality. The reference
  output is only useful to understand what the tree-sitter grammar produces; do not
  paste it into the `.test` file as a `stdout:` golden. If you do, the test will fail
  every time the tree shapes differ (they will — tinyparse's CST and tree-sitter's
  are not identical).
- *One file per corpus section vs. multi-section files.* If you keep multiple corpus
  sections in one input file, a single ERROR anywhere in the file makes the whole test
  fail. Splitting each section into its own file gives finer-grained xfail tracking.
- *Encoding issues.* Some reference corpora include UTF-8 inputs or unusual whitespace.
  Write the file bytes verbatim; do not re-encode or strip.
- *Dialect-specific syntax in the base grammar corpus.* A tree-sitter-sql corpus
  may mix Standard SQL, PostgreSQL extensions, and SQLite extensions in one directory.
  Sort inputs by which dialect can parse them and assign them to the right
  `test/suite/<dialect>inputs/` subdirectory.

---

## Step 7 — Write test cases in `test/suite/<family>/`

One `.test` file per input file. The standard shape for a "parses cleanly" case:

```
<<<cmd
"$TP" run std.<name> --input test/suite/<family>/inputs/<name>.sql --dump-tree
<<<assert
exit 0
out-absent ERROR
out-absent MISSING
```

For a "rejects a deliberately malformed input" case:

```
<<<cmd
"$TP" run std.<name> --input test/suite/<family>/inputs/<name>_bad.sql --dump-tree
<<<assert
exit-not 0
```

Or a structural assertion (verifies the tree shape, not just acceptance):

```
<<<cmd
"$TP" run std.<name> --input test/suite/<family>/inputs/<name>.sql --dump-tree
<<<assert
exit 0
out-absent ERROR
out-order select_statement ~ from_clause ~ where_clause
out-count 2 join_clause
```

**Failure modes**

- *Referencing `std.<name>` by package name before the grammar is embedded.* The CLI
  resolves package names from the embedded registry `mise run generate` writes.
  Until you run `mise run build`, the name does not exist.
  Use a file path instead during early authoring:
  `"$TP" run grammars/std/<name>/grammar.scm --input …`
- *`$TP` path wrong in `cmd`.* The harness sets `$TP` to the built CLI. If the build
  has not run at all, every test in the suite will fail with "command not found".
  Run `mise run build` once before running the metric.
- *Test name collision.* A test's identity is its path below `suite/` without the
  `.test` extension. If you create `suite/sqlfamily/expressions__1.test` and there is
  already a `suite/jsfamily/expressions__1.test`, they are distinct (different paths).
  But if you accidentally reuse a name within the same subdirectory, the second entry
  overwrites the first in the harness — you will see fewer tests than you created.

---

## Step 8 — Seed `test/xfail.list` with every new test

Before running the metric, add every new test name to `test/xfail.list`:

```
# std.sql / std.sqlite / std.postgres — W-next
sqlfamily/expressions__1
sqlfamily/expressions__2
sqlfamily/declarations__1
…
```

The ratchet rule: any test that fails and is **not** in `xfail.list` is a
regression (FAIL) and the metric exits non-zero. Seed all of them before the
first metric run.

**Failure modes**

- *Forgetting to seed.* The first `mise run test` after adding tests fails with
  `FAIL` entries and a red metric. Add all the new names to `xfail.list` and re-run.
- *Seeding tests that already pass.* A test in `xfail.list` that passes is
  reported as `XPASS` (unexpected pass) and the metric also exits non-zero. If a
  test passes immediately (e.g. a trivially-accepted input), remove it from
  `xfail.list` right away.
- *Misspelled test name in `xfail.list`.* The harness matches by exact string. A
  typo means the entry never matches anything — the test stays `FAIL`. Compare the
  harness's printed test name character-by-character against `xfail.list`.

---

## Step 9 — Build and verify the grammar resolves

```sh
mise run build
tinyparse grammars      # check std.<name> appears in the list
tinyparse check grammars/std/<name>/grammar.scm   # compile the grammar, report errors
```

Then run a quick smoke test against a known-good input before touching the metric:

```sh
echo "SELECT 1;" | tinyparse run std.<name> --dump-tree
```

**Failure modes**

- *Grammar compile error in `check`.* The error message names the offending token
  or rule and the reason. Common causes: duplicate name, undefined reference (a rule
  that calls a token or rule that was not defined), unbalanced parentheses in the
  `.scm` reader, bad regex pattern.
- *Grammar not listed by `tinyparse grammars`.* `mise run generate` scans
  `grammars/std/*/package.scm`; a package without both `package.scm` and
  `grammar.scm` is skipped. Check that `build/generated/grammars_data.h` was
  regenerated (it is a generated file — look for your grammar's name inside it).
- *`extends:` chain unresolved at runtime.* Even if `package.scm` is syntactically
  correct, the runtime resolver must find every package in the chain by name in the
  embedded registry. If `std.<dialect>` extends `std.<base>` and `std.<base>` was
  not embedded (perhaps because its `package.scm` had a reader error that silently
  skipped it), the dialect will fail with "package not found" at grammar-compile time.
  Run `tinyparse grammars` to verify every member of the chain is listed.

---

## Step 10 — Work tests off `xfail.list` one category at a time

Run the metric:

```sh
mise run test
```

Pick a failing category (e.g. `sqlfamily/expressions`), run those tests
individually to read the tree output, fix the grammar rule responsible, re-run:

```sh
tinyparse run grammars/std/<name>/grammar.scm \
  --input test/suite/<family>/inputs/expressions__1.sql --dump-tree
```

When a test passes, delete its line from `xfail.list`. The metric's
`PROGRESS=k/n` counter only rises; a drop means a previously-passing test broke.

**Failure modes**

- *Fixing one rule breaks another.* A rule change that makes `expressions__3` pass
  may make `declarations__5` fail. After each grammar edit, run the full metric
  rather than individual tests — a drop in PROGRESS is the signal.
- *`ERROR` node in the tree but exit code 0.* tinyparse exits 0 even when it
  recovers from parse errors (the tree contains `ERROR` or `MISSING` nodes). The
  test asserts `out-absent ERROR` — check the dump for recovery nodes, not just the
  exit code.
- *Test passes locally but remains in `xfail.list`.* An `XPASS` is also a metric
  failure. Delete the line from `xfail.list` as soon as the test passes.
- *A corpus input is beyond the grammar's current scope.* Some reference corpus
  sections cover features you have not implemented yet. It is correct to leave those
  in `xfail.list` while you work on them; do not delete them from the suite. The
  ratchet tracks pending work, not only regressions.

---

## Step 11 — Verify all three correctness gates

Once PROGRESS reaches the full count and `xfail.list` is empty:

```sh
mise run test                        # conformance green
mise run gate vm-diff                # bytecode VM == recursive oracle
mise run gate cc-diff                # generated C parser == bytecode VM
```

The differential gates ensure that every engine produces byte-identical trees
for the new grammar. A grammar change that alters the tree must update goldens
deliberately; a change claimed to be performance-only must leave all three green.

**Failure modes**

- *vm-diff or cc-diff fails only for the new grammar.* The grammar compiled to
  different bytecode for the VM path vs the recursive path. This usually means a
  rule that exercises a code path not yet covered by the other engine — report it
  as a differential divergence and investigate rather than masking.
- *cc-diff has no generated parser yet for the new grammar.* The generated C
  parser must be explicitly produced with `tinyparse c --parser-lib` targeting
  the new grammar. If it has not been generated, `cc-diff` cannot run it — either
  generate it (and add it to the diff corpus) or mark the diff gate as pending for
  this grammar until the grammar has stabilised.

---

## Step 12 — Left-factor hot rules

Run a timing check with the rule memo disabled over a deeply-nested input:

```sh
TINYPARSE_RULEMEMO=off tinyparse run grammars/std/<name>/grammar.scm \
  --input <deeply-nested.sql> --dump-tree
```

If this is pathologically slow (seconds on a small file), the grammar has a
re-parsing choice that the memo hides. Use the **left-factor skill** to identify
and fix it.

The expression precedence ladder (step 4) is the usual culprit. SQL's
`ALTER TABLE … ADD COLUMN | DROP COLUMN | RENAME …` is another: all three
branches share the `ALTER TABLE <name>` prefix and re-parse it on backtrack.
Factor the shared prefix into one `(seq "ALTER" "TABLE" table_name (choice …))`
before declaring the grammar settled.

**Failure modes**

- *Memo hides the problem.* With the memo on, a deeply-nested file parses fast
  even with an unfactored grammar — the cached result short-circuits the re-parse.
  Always measure with `TINYPARSE_RULEMEMO=off` to see the grammar's actual cost.
  Only disable the memo for this measurement; leave it on for production.
- *Factoring shifts the tree shape.* An assignment's LHS may become a
  `ternary_expression` node instead of a dedicated `lhs` wrapper after factoring.
  This is acceptable — tinyparse conformance is accept-level, not tree-equality.
  Verify that the corpus still accepts/rejects the same inputs, not that every node
  name is unchanged.

---

## Reference: file checklist for a new grammar family

```
grammars/std/<base>/package.scm        — manifest for the base dialect
grammars/std/<base>/grammar.scm        — base grammar source
grammars/std/<dialect>/package.scm     — manifest for each dialect
grammars/std/<dialect>/grammar.scm     — dialect extension source

reference/PINS.txt                     — new entry (+ re-fetch commands)

test/suite/<family>/inputs/*.sql       — corpus inputs (one per corpus section)
test/suite/<family>/*.test             — one test file per input

test/xfail.list                        — every new test name added before first run
```

After committing, rebase onto `origin/main` if it has moved and push. Each
grammar family is one logical commit; the differential and conformance gates must
be green before the commit lands.
