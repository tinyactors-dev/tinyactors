# jit: generate x86-64 code from a tree

`jit.c` parses a function written in a small subset of C, generates x86-64
machine code for it while walking the concrete syntax tree (CST), and calls
the code with an integer argument. It shows code generation driven by the
tree itself: the walk dispatches on node kinds and field names and emits
instructions as it goes, with no intermediate data structure between the
tree and the machine code. It uses the
[tree cursor](../../docs/reference/c-api.md#tree-cursor) functions and a
grammar built from the [`token`](../../docs/reference/grammar.md#tokens),
[`seq`, `choice`, `repeat`, `optional`](../../docs/reference/grammar.md#rules)
and [`field`](../../docs/reference/grammar.md#field) forms.

## Prerequisites

Run the commands from the `tinyparse/` directory after `mise run examples`,
as [Examples](../README.md#prerequisites) describes.

## Run it

`run.sh` takes the C source, the name of the function to call and its
integer argument. Square 7 and add 1:

```sh
examples/jit/run.sh 'int f(int x){return x*x+1;}' f 7
```

The program prints the result and exits with status 0:

```text
50
```

A function can call itself. The tenth Fibonacci number:

```sh
examples/jit/run.sh 'int fib(int n){return n<2?n:fib(n-1)+fib(n-2);}' fib 10
```

```text
55
```

## How it works

The grammar, `jit.scm`, reads one or more functions of the form
`int NAME(int PARAM, ...) { return EXPR; ... }`, with integer literals,
identifiers, calls, parentheses, prefix `-`, `*`, `+`, `-`, `<` and `?:`.
Precedence comes from a ladder of rules from `Ternary` down to `Primary`.
Each binary level is `(field lhs: ...)` followed by
`(repeat (seq (field op: ...) (field rhs: ...)))`, so `a - b - c` is one node
with the children `lhs`, `op`, `rhs`, `op`, `rhs`.

`emit_expr` handles the leaves by node kind: a `Number` loads its value, an
`Ident` loads the parameter, and a `Call` calls the function. Every other
node goes to `emit_rule`, which visits the node's children once, in order,
and acts on each child's field name:

| Field | Code emitted |
|-------|--------------|
| `lhs` | the operand |
| `op` | nothing; the operator is remembered |
| `rhs` | the operand, then the remembered operator applied to the two values on the stack |
| `cond`, `then`, `else` | the condition and a conditional jump, the then-branch and a jump past the else-branch, the else-branch; both jump offsets are filled in when their targets are known |
| `neg` | nothing; the operand that follows has no field, and its value is negated after the walk |
| `num`, `var`, `group`, `expr` | the literal, the parameter, or the nested expression |
| no field | the child, when it has children and no value is on the stack yet |

Folding each `op` and `rhs` pair into the value on the stack, in sibling
order, evaluates the chain left to right. The text of a literal or operator
is the input at the node's byte range.

The generated code is a stack machine in the System V x86-64 calling
convention. The argument arrives in `edi` and is stored at `-8(%rbp)`, and
each expression pushes its 32-bit result. A call moves its argument into
`edi` and calls offset 0 of the code buffer. `run_jit` writes the code into a
readable and writable page, then makes it readable and executable with
`mprotect` before calling it.

## Limits

The grammar accepts more than the code generator handles correctly.

- Every parameter and every identifier reads the same stack slot, which holds
  the first argument. `int f(int a, int b){return a-b;}` called with 7 prints
  `0`, and `int f(int x){return y+1;}` called with 7 prints `8`; both exit
  with status 0.
- A call always jumps to the function being compiled.
  `int g(int x){return x+1;} int f(int x){return g(x)*2;}` called as `f`
  recurses until the stack overflows, and the program dies with a
  segmentation fault, exit status 139.
- A call passes one argument. A call with two or more arguments
  unbalances the stack: `int f(int x){return x<1?0:f(x-1,5)+1;}` called as
  `f 3` dies with a segmentation fault, exit status 139. A call with no
  arguments passes 0: `int f(int x){return x<1?0:f()+1;}` called with 3
  prints `1`.
- A function with several `return` statements returns the value of the last
  one: `int f(int x){return 1; return 2;}` prints `2`.
- `/`, `>` and every other operator are syntax errors: the program prints
  `jit: parse: error: unexpected input` and `jit: could not parse the C
  source`, and exits with status 2.
- Arithmetic is 32-bit and wraps: `2147483647+x` called with 1 prints
  `-2147483648`.
- The argument is read with `atoi`, so `abc` is passed as 0.
- The code buffer holds 4,096 bytes. A longer function stops with
  `jit: code buffer overflow` and exit status 2; 500 additions in one
  expression are enough.
- The program finds its grammar through the environment variable
  `JIT_GRAMMAR`, which `run.sh` sets, and otherwise at the relative path
  `examples/jit/jit.scm`. Run directly from another directory without
  `JIT_GRAMMAR`, it prints `jit: cannot read grammar 'examples/jit/jit.scm'`
  and exits with status 2.
- `jit.c` stops at compile time on a host that is not x86-64.

## Files

| File | Contents |
|------|----------|
| `jit.scm` | the grammar for the C subset |
| `jit.c` | the program: parse, generate code from the tree, call it |
| `run.sh` | runs the program built by `mise run examples` with `JIT_GRAMMAR` set |
