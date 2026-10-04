# import — module import across `.scm` files

This example shows pocketparse's `(import NAME)` form: a grammar that pulls
token, rule, and `extras` definitions out of a second `.scm` module and uses
them by name.

- [`words.scm`](words.scm) is the module. It defines a `Word` token (a run of
  ASCII letters), a `Space` token, and a whitespace `(trivia Space)` extras
  policy.
- [`main.scm`](main.scm) is the importing grammar. It does `(import words)` and
  defines a `Source` rule of `(repeat Word)` — referencing `Word` even though
  it is never declared in `main.scm` itself.

## How it resolves

The pocketparse core has no filesystem, so it never reads `words.scm` itself.
When the compiler meets `(import words)` it asks the **host's module resolver**
for the module's bytes (`pocketparse_set_module_resolver`). The CLI installs a
filesystem-backed resolver that maps the module name to a file relative to the
importing grammar's directory: `words` → `words.scm`, and a dotted name like
`a.b` → `a/b.scm`. The resolved bytes are compiled and their definitions are
**merged** into the grammar being built, so `Word` is in scope by the time
`Source` names it.

Resolution happens entirely at **compile time**: the compiled grammar (and the
packed `.ppg` it would serialize to) is identical to one where `Word` had been
written in `main.scm` directly. Import cycles are detected and rejected, and a
grammar that imports anything without a resolver installed fails with a clear
diagnostic.

## Run it

```sh
printf 'a b c' | pocketparse run examples/import/main.scm --input - --dump-tree
```

The input `a b c` parses to a `Source` of three `Word` nodes, with the
whitespace consumed as trivia.
