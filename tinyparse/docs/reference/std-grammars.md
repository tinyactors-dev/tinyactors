# Standard grammars

This reference lists the grammars built into the `tinyparse` command, for users
who parse JSON, JavaScript, TypeScript, POSIX shell or fenced code blocks with
them and need to know what each accepts and where each is wrong.

Commands on this page assume the current directory is the `tinyparse/`
directory of a source checkout and that `tinyparse` is on `PATH`
([Install tinyparse](../how-to/install.md)).

## Common facts

| Name | Package directory | Extends | Start rule | Language |
|------|-------------------|---------|------------|----------|
| `std.json` | `grammars/std/json/` | | `Document` | JSON |
| `std.jsonc` | `grammars/std/jsonc/` | `std.json` | `Document` | JSON with comments |
| `std.json5` | `grammars/std/json5/` | `std.jsonc` | `Document` | part of JSON5 |
| `std.javascript` | `grammars/std/javascript/` | | `program` | JavaScript with JSX |
| `std.typescript` | `grammars/std/typescript/` | `std.javascript` | `program` | TypeScript |
| `std.tsx` | `grammars/std/tsx/` | `std.typescript` | `program` | TypeScript with JSX |
| `std.shell` | `grammars/std/shell/` | | `Program` | POSIX sh |
| `std.fence` | `grammars/std/fence/` | | `Document` | fenced `js` and `json` code blocks |

Each grammar is a [grammar package](grammar.md#packagescm-manifest): a
`package.scm` manifest and a `grammar.scm` grammar. The `tinyparse` command
carries a copy of every package and accepts the name wherever it accepts a
grammar; `tinyparse grammars` lists the names. A file or package directory with
the same path as the name is used instead of the built-in copy. A C host compiles a
standard grammar from its `grammar.scm` and needs a module resolver for one
that extends another
([`tinyparse_set_module_resolver`](c-api.md#tinyparse_set_module_resolver),
[Supply imports, classes and parameters from a C host](../how-to/host-services.md));
the memory a compile needs is under [Sizing a grant](c-api.md#sizing-a-grant).

`tinyparse node-types NAME` prints every node kind a grammar can produce, with
its fields, as JSON ([node-types in tinyparse(1)](../../tinyparse.1.md#node-types)).

Corpus cases are accept-level: a case passes when `tinyparse run`
exits with status 0 and the tree holds no `ERROR` or `MISSING` node. The tree is
not compared with the tree of the grammar the case came from. The reasoning is
recorded in [the decision log](../decisions.md). The third-party corpora are
pinned by commit in [`reference/PINS.txt`](../../reference/PINS.txt); their
inputs are copied into `test/suite/`.

Each limitation states how it fails: with a diagnostic and exit status 1, or
silently, with a wrong tree and exit status 0.

## std.json

- **Package:** `grammars/std/json/`
- **Extends:** none
- **Parameters:** none
- **Start rule:** `Document`: one value, then the end of input. Whitespace
  (space, tab, carriage return, line feed) is `Space` trivia.
- **Accepts:** objects, arrays, strings, numbers, `true`, `false` and `null`.
- **Conformance:** no corpus; `test/suite/query/json-base.test` parses one
  document.

| Input | Result |
|-------|--------|
| `01` (leading zero) | Accepted, exit status 0. |
| `"\q"` (an escape JSON does not define) | Accepted, exit status 0. A backslash may precede any byte; `\u` escapes are not checked. |
| a tab or other control byte inside a string | Accepted, exit status 0. |
| `[1,]`, `{"a":1,}`, `1.`, `.5`, `+1` | Rejected, exit status 1. |

## std.jsonc

- **Package:** `grammars/std/jsonc/`
- **Extends:** `std.json`
- **Parameters:** none
- **Start rule:** `Document`
- **Accepts:** `std.json` plus `//` line comments and `/* */` block comments
  wherever whitespace may appear. Both are `Comment` trivia.
- **Conformance:** no corpus; `test/suite/query/jsonc.test` parses one document.

| Input | Result |
|-------|--------|
| The `std.json` limitations | The same results. |
| `/*` with no closing `*/` | Rejected, exit status 1. |

## std.json5

- **Package:** `grammars/std/json5/`
- **Extends:** `std.jsonc`
- **Parameters:** none
- **Start rule:** `Document`
- **Accepts:** `std.jsonc` plus unquoted object keys (`[A-Za-z_$]` then
  `[A-Za-z0-9_$]*`, an `Identifier` node) and a trailing comma after the last
  object member. The rest of JSON5 is not implemented.
- **Conformance:** no corpus; `test/suite/query/json5.test` parses one document.

| Input | Result |
|-------|--------|
| The `std.json` limitations | The same results. |
| `[1,]` (trailing comma in an array) | Rejected, exit status 1. |
| `{'a':1}` (single-quoted string) | Rejected, exit status 1. |
| `0x1F`, `Infinity`, `+1`, `.5` | Rejected, exit status 1. |

## std.javascript

- **Package:** `grammars/std/javascript/`
- **Extends:** none
- **Parameters:**

  | Parameter | Manifest value | Effect |
  |-----------|----------------|--------|
  | `jsx` | `#t` | None in this grammar: JSX is always accepted. `std.typescript` reads it. |
  | `types` | `#f` | None: no standard grammar reads it. |

- **Start rule:** `program`
- **Accepts:** JavaScript statements and expressions, JSX, template literals,
  regular-expression literals, hash-bang lines, `<!--` comments. Node names
  follow tree-sitter-javascript where the two agree. `//`, `/* */` and `<!--`
  comments are trivia with `attach: leading`
  ([Extras](grammar.md#extras)).
- **Regex or division:** the lexical goal `expr` reads `/` as a `regex` token
  where an expression starts; the goal `div` reads it as the `Div` operator
  ([Lexical goals](grammar.md#lexical-goals)).
- **Semicolons:** each statement that JavaScript ends with `;` ends in a `semi`
  node holding an optional `;`.
  Where the source omits it, `semi` is an empty node with a zero-width range:

  ```sh
  printf 'let a = 1\nlet b = 2' > input.js
  tinyparse run std.javascript --input input.js --dump-tree
  ```

  Both statements end in an empty `semi`. The `Space` nodes and the
  declarators' children are cut from this output:

  ```text
  program [0-19] [0,0]-[1,9]
    lexical_declaration [0-10] [0,0]-[1,0]
      kind: let [0-3] [0,0]-[0,3]
      ...
      variable_declarator [4-9] [0,4]-[0,9]
      ...
      semi [10-10] [1,0]-[1,0]
    lexical_declaration [10-19] [1,0]-[1,9]
      kind: let [10-13] [1,0]-[1,3]
      ...
      variable_declarator [14-19] [1,4]-[1,9]
      ...
      semi [19-19] [1,9]-[1,9]
  ```

- **Conformance:** the tree-sitter-javascript corpus files `destructuring`,
  `expressions`, `literals`, `semicolon_insertion`, `statements` and
  `injectables`, in `test/suite/jsfamily/`. Every case is an accept test except
  `destructuring__5`, a malformed input that must be rejected.

| Input | Result |
|-------|--------|
| An identifier that starts with `let`, `const`, `var`, `new`, `typeof`, `void`, `delete`, `await`, `yield`, `return`, `throw`, `debugger`, `this`, `super`, `true`, `false`, `null` or `undefined`, at the start of a statement | Split after the keyword, exit status 0: `letter = 1;` is `let ter = 1;`, `newValue = 1;` is `new Value = 1;`, `trueish = 1;` is `true` followed by `ish = 1;`. |
| An identifier that starts with `new`, `typeof`, `void`, `delete`, `await`, `yield`, `this`, `super`, `true`, `false`, `null` or `undefined`, on the right of `=` | Split, exit status 0: `x = newValue;` holds a `new_expression`; `x = trueish;` is `x = true` followed by the statement `ish;`. |
| An identifier that starts with `this`, `super`, `true`, `false`, `null` or `undefined`, as a call argument | Rejected, exit status 1: `f(thisArg);`. |
| An identifier that starts with `break` or `continue`, at the start of a statement | Rejected, exit status 1. |
| `return`, a line break, `x` | Read as `return x`, exit status 0. JavaScript ends the `return` at the line break. |
| `x`, a line break, `++y` | Read as `x++` followed by `y`, exit status 0. JavaScript reads `x; ++y`. |

## std.typescript

- **Package:** `grammars/std/typescript/`
- **Extends:** `std.javascript`
- **Parameters:**

  | Parameter | Manifest value | Effect |
  |-----------|----------------|--------|
  | `jsx` | `#f`, which sets the parameter ([package.scm manifest](grammar.md#packagescm-manifest)) | When set, a JSX element is a primary expression. Pass `--param jsx=0` to reject JSX. |
  | `types` | `#t` | None: no standard grammar reads it. |

- **Start rule:** `program`
- **Accepts:** `std.javascript` plus type annotations, generics, interfaces,
  type aliases, enums, namespaces, the type grammar, and `as`, `satisfies` and
  `<T>expr` type assertions.
- **Conformance:** the tree-sitter-typescript corpus files `declarations`,
  `expressions`, `functions` and `types`, and hand-written cases, all in
  `test/suite/tsfamily/`; cases the corpus marks for TSX run against
  `std.tsx`. Corpus cases are accept tests; a few also assert node counts where
  an accepted input could hide a wrong tree. Hand-written cases also check
  node counts and placement of comments, and reject 3,000 unclosed `(` with
  exit status 1 within a 20-second CPU limit.

| Input | Result |
|-------|--------|
| The `std.javascript` limitations | The same results. `typeZ = 1;` is additionally read as the type alias `type Z = 1`, exit status 0. |
| `x = <a>hi</a>;` with no `--param` | Accepted as JSX, exit status 0. |
| `let x = <T>y;` (type assertion after `=`) | Rejected, exit status 1. `let x = y as T;` is accepted. |
| `f(<T>y);` (type assertion as an argument) | Read as two statements, `f` and `(<T>y)`, exit status 0. |
| An identifier that starts with `namespace` or `module`, at the start of a statement | Rejected, exit status 1. |

## std.tsx

- **Package:** `grammars/std/tsx/`
- **Extends:** `std.typescript`
- **Parameters:**

  | Parameter | Manifest value | Effect |
  |-----------|----------------|--------|
  | `jsx` | `#t` | JSX elements are primary expressions. `--param jsx=0` rejects them. |
  | `types` | `#t` | None: no standard grammar reads it. |

- **Start rule:** `program`
- **Accepts:** `std.typescript` with JSX, generic JSX tags (`<Foo<T> />`), and
  without the `<T>expr` type assertion: a leading `<` opens a JSX element, and a
  cast is written `x as T`.
- **Conformance:** the tree-sitter-typescript corpus cases marked for TSX, and
  a hand-written case that rejects 3,000 unclosed `(` with exit status 1
  within a 20-second CPU limit, in `test/suite/tsfamily/`.

| Input | Result |
|-------|--------|
| The `std.javascript` and `std.typescript` limitations, except those about `<T>expr` | The same results. |

## std.shell

- **Package:** `grammars/std/shell/`
- **Extends:** none
- **Parameters:** none
- **Start rule:** `Program`
- **Accepts:** the POSIX sh command language: simple commands with assignment
  prefixes and redirection operators, pipelines, `!`, `&&` and `||` lists, `;` and `&`
  separators, `if`, `for`, `while`, `until`, `case`, subshells, brace groups,
  function definitions, quoting, the expansions `$name`, `${…}`, `$(…)`, `` `…` ``
  and `$((…))`, and here-documents with `<<` and `<<-`. A line feed ends a
  command and is a `Newline` token; spaces, tabs and `#` comments are trivia.
  A command name is a `CommandName` node holding a `ReservedWord` leaf.
  Despite its name, `ReservedWord` is the bare word that is not a reserved
  word: in `echo hi`, `echo` is a `ReservedWord`.
- **Conformance:** the POSIX sections of the tree-sitter-bash corpus files
  `commands`, `statements`, `literals` and `programs`, as accept tests, and
  hand-written cases that check acceptance, tree structure, or rejection (exit
  status other than 0), all in
  `test/suite/shellfamily/`. A corpus section is copied only if it is POSIX sh
  and `std.shell` accepts it.

Bash and ksh constructs are outside the POSIX scope. They are not implemented,
and some are accepted because their POSIX reading parses:

| Construct | Example | Result |
|-----------|---------|--------|
| `[[ … ]]` | `[[ $x == y ]]` | Exit status 0: `[[` is a command name with four arguments. |
| `(( … ))` | `(( i++ ))` | Exit status 0: two nested subshells. |
| `$'…'` | `echo $'a'` | Exit status 0: a `$` followed by a single-quoted string. |
| Brace expansion | `echo {a,b}`, `echo {0..5}` | Exit status 0: one word. |
| `$[ … ]` | `echo $[a + 1]` | Exit status 0: three words, `$[a`, `+` and `1]`. |
| `&>` | `ls &>/dev/null` | Exit status 0: `ls &`, then the command `>/dev/null`. |
| `;&` in `case` | `case x in a) echo ;& esac` | Exit status 0: `;` and `&` read separately. |
| `declare`, `local`, `export`, `readonly`, `unset` | `declare -i x=1` | Exit status 0: an ordinary command with arguments, as POSIX reads it. |
| Arrays | `a=(1 2 3)`, `echo ${a[1]}` | Rejected, exit status 1. |
| C-style `for` | `for (( i=0; i<5; i++ )); do echo; done` | Rejected, exit status 1. |
| `select` | `select x in a b; do echo; done` | Rejected, exit status 1. |
| Process substitution | `diff <(ls) <(ls)` | Rejected, exit status 1. |
| Here-string | `cat <<< word` | Rejected, exit status 1. |
| `function` keyword | `function f { echo; }` | Rejected, exit status 1. |
| `time` keyword | `time ( ls )` | Rejected, exit status 1. |
| `\|&` | `ls \|& cat` | Rejected, exit status 1. |
| Substring, case, and pattern expansions | `${v:0:1}`, `${v^^}`, `${!v}`, `${v/a/b}` | Rejected, exit status 1. |

POSIX constructs `std.shell` gets wrong:

| Input | Result |
|-------|--------|
| A quoted here-document delimiter: `<<'EOF'`, `<<"EOF"`, `<<\EOF` | Rejected, exit status 1. |
| A here-document whose closing line is the last line and has no line feed | Exit status 0: the body is not attached to the command, and its lines parse as ordinary commands. |
| A here-document with no closing line | Exit status 0, as above. |
| `$12` | Exit status 0: one `SpecialParam` spanning `12`. POSIX reads `${1}2`. |
| `$((a)+(b))` | Exit status 0: read as a command substitution `$(…)` holding two subshells. |

## std.fence

- **Package:** `grammars/std/fence/`
- **Extends:** none
- **Parameters:** none
- **Start rule:** `Document`: one or more fenced blocks, separated by
  whitespace.
- **Accepts:** blocks that open with ```` ```js ```` or ```` ```json ````
  (optionally followed by spaces or tabs) and a line feed, and close with a line
  feed and ```` ``` ````. The content of a `js` block is parsed by
  `std.javascript`, of a `json` block by `std.json`, through
  [`inject`](grammar.md#inject). Each block's tree sits under an `injection`
  node whose nodes carry the language tag, printed as `[std.json]` by
  `--dump-tree`.
- **Conformance:** `test/suite/inject/`.

| Input | Result |
|-------|--------|
| Content the injected grammar rejects, such as `{"a":` in a `json` block | Exit status 0, no diagnostic: the `injection` node holds an empty root node. |
| Any other info string, such as ```` ```python ```` | Rejected, exit status 1. |
| Text outside a fence | Rejected, exit status 1. |
| A closing ```` ``` ```` on the line after the opening line | Rejected, exit status 1. |
| One empty content line between the opening and closing lines | Exit status 0: the `injection` node spans zero bytes and holds an empty root node. Empty input is a valid `std.javascript` program; `std.json` rejects it without a diagnostic, as in the first row. |
| `~~~` fences | Rejected, exit status 1. |
