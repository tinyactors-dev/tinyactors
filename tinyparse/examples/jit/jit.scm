; jit.scm — a grammar for a sane subset of C, just enough to JIT the demo
; functions in this example: int-returning functions of int parameters whose
; bodies are a single `return <expr>;`.
;
; The expression grammar is layered so precedence falls out of the rule shape,
; lowest precedence at the top:
;
;   Ternary   ?:            (right-associative, lowest)
;   Relational  <
;   Additive   + -          (left-associative)
;   Multiplicative *        (left-associative)
;   Unary      -            (prefix)
;   Primary    literals, identifiers, calls, ( … )
;
; The parser is the canonical pocketparse one: layered rules give precedence,
; (choice …) picks a form, (repeat …) gathers the tails of left-associative
; chains, and (field …) labels the parts the codegen walker cares about.

(pocketparse version: "0.1")

(module examples.jit)

(grammar cjit
  start: Unit

  (token Space   (regex "[ \t\r\n]+"))
  (token Int     "int")
  (token Return  "return")
  (token Number  (regex "[0-9]+"))
  (token Ident   (regex "[A-Za-z_][A-Za-z0-9_]*"))

  (extras
    (trivia Space))

  ; A translation unit is one or more functions.
  (rule Unit
    (seq (repeat Function min: 1) EOF))

  ; int NAME ( params ) { body }
  (rule Function
    (seq
      Int
      (field name: Ident)
      "("
      (optional Params)
      ")"
      "{"
      (field body: (repeat Statement min: 1))
      "}"))

  (rule Params
    (seq Param (repeat (seq "," Param))))

  (rule Param
    (seq Int (field pname: Ident)))

  ; The only statement in the subset.
  (rule Statement
    (seq Return (field value: Expr) ";"))

  ; Expression precedence, low to high.
  (rule Expr
    (field expr: Ternary))

  (rule Ternary
    (choice
      (seq
        (field cond: Relational)
        "?"
        (field then: Expr)
        ":"
        (field else: Ternary))
      Relational))

  (rule Relational
    (seq
      (field lhs: Additive)
      (repeat (seq (field op: "<") (field rhs: Additive)))))

  (rule Additive
    (seq
      (field lhs: Multiplicative)
      (repeat (seq (field op: (choice "+" "-")) (field rhs: Multiplicative)))))

  (rule Multiplicative
    (seq
      (field lhs: Unary)
      (repeat (seq (field op: "*") (field rhs: Unary)))))

  (rule Unary
    (choice
      (seq (field neg: "-") Unary)
      Primary))

  (rule Primary
    (choice
      (field num: Number)
      Call
      (field var: Ident)
      (seq "(" (field group: Expr) ")")))

  ; A function call: NAME ( args ).  Disambiguated from a bare identifier by
  ; the trailing "(".
  (rule Call
    (seq
      (field callee: Ident)
      "("
      (optional CallArgs)
      ")"))

  (rule CallArgs
    (seq (field arg: Expr) (repeat (seq "," (field arg: Expr))))))
