# bindings: match characters with a host class

`ident.scm` reads identifiers built from `(class alpha)` and `(class alnum)`,
character classes whose bytes come from the program that runs the parser. The
`tinyparse` command supplies four classes. The example uses the
`(class NAME)` form described under
[Classes](../../docs/reference/grammar.md#classes) and the function behind it,
[`tinyparse_bind`](../../docs/reference/c-api.md#tinyparse_bind).

## Prerequisites

Run the commands from the `tinyparse/` directory with `tinyparse` on your
`PATH`, as [Examples](../README.md) describes.

## Run it

Parse three identifiers:

```sh
printf 'foo bar123 x' | tinyparse run examples/bindings/ident.scm --input - --dump-tree
```

Each `Ident` covers one identifier, and the spaces between them are trivia:

```text
Source [0-12] [0,0]-[0,12]
  Item [0-3] [0,0]-[0,3]
    Ident [0-3] [0,0]-[0,3]
  Space [3-4] [0,3]-[0,4]
  Item [4-10] [0,4]-[0,10]
    Ident [4-10] [0,4]-[0,10]
  Space [10-11] [0,10]-[0,11]
  Item [11-12] [0,11]-[0,12]
    Ident [11-12] [0,11]-[0,12]
```

An identifier must start with a letter:

```sh
printf '9lives' | tinyparse run examples/bindings/ident.scm --input - --dump-tree
```

The command prints a diagnostic, no tree, and exits with status 1:

```text
error: unexpected input [parse.syntax]
```

## How it works

The `Ident` token is the only place the grammar uses a class:

```scheme
(token Ident
  (seq
    (class alpha)
    (repeat (class alnum))))
```

- `(class alpha)` matches one input byte when the function bound to the name
  `alpha` accepts it. `(repeat (class alnum))` matches as many bytes as the
  `alnum` function accepts in a row.
- The grammar does not say which bytes those are. The `tinyparse` command binds
  `alpha`, `digit`, `alnum` and `space` before every command that compiles a
  grammar; their bytes are listed under
  [Classes](../../docs/reference/grammar.md#classes).
- A C program has no classes until it binds them with `tinyparse_bind`.
  [Supply imports, classes and parameters from a C host](../../docs/how-to/host-services.md)
  binds the same four and a class of its own.
- The parser looks a class up by name each time it needs one, so a binding
  made after the grammar compiled still applies.

## Limits

- The four classes are ASCII only. `printf 'café'` fails with
  `error: unexpected input [parse.syntax]` at the first byte of `é`, and the
  command exits with status 1.
- A class that nothing binds matches no byte and raises no compile fault. A
  grammar that uses `(class xid-start)` passes `tinyparse check` with status 0,
  and every input that needs the class then fails with `parse.syntax`.
- The programs that `tinyparse c --parser` and `tinyparse c --bundle` write
  from `ident.scm` bind no class: run on `foo`, each prints
  `error: unexpected input` and exits with status 1. A program that runs this
  grammar outside the `tinyparse` command binds the classes itself.

## Files

| File | Contents |
|------|----------|
| `ident.scm` | the grammar; `test/suite/bindings/class-builtin.test` runs it |
