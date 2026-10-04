; mini-js.scm — parser-directed lexical goals: the JavaScript `/` problem.
;
; A single source byte, `/`, means two different things depending on where the
; parser is. At the start of an expression it opens a regular-expression literal
; (`/ab/`); after a value it is the division operator (`c / d`). No amount of
; rule ordering settles this on its own — the SAME lexeme must scan as two
; different tokens — so the grammar selects a *lexical goal* and the scanner
; consults it.
;
; The mechanism has two halves:
;
;   (lex-goal GOAL trigger: Target ...)   ; in GOAL, scan `trigger` as `Target`
;   (goal GOAL X)                         ; make GOAL active while matching X
;
; `Slash` is the ambiguous trigger token (a bare `/`). In the `expr` goal a
; reference to `Slash` scans as `RegexLiteral`; in the `op` goal it scans as
; `Div`. A rule picks the goal with `(goal expr …)` / `(goal op …)`, and the
; executor redirects the trigger's scan to the goal's target token, emitting the
; leaf under the target's kind.
;
; So for `a = /ab/ ;` the right-hand side is read in the `expr` goal and `/ab/`
; becomes a RegexLiteral; for `b = c / d ;` the `/` between two values is read in
; the `op` goal and becomes a Div.

(tinyparse version: "0.1")

(module examples.lexgoal.mini-js)

(grammar mini-js
  start: Source

  (token Space (regex "[ \t\r\n]+"))
  (token Ident (regex "[A-Za-z]+"))
  (token Eq    "=")
  (token Semi  ";")

  ; The ambiguous lexeme and the two tokens a goal redirects it to. `slash` is
  ; never emitted as a node — every goal redirects it to RegexLiteral or Div.
  (token slash        "/")
  (token RegexLiteral (regex "/[^/]*/"))
  (token Div          "/")

  ; In the `expr` goal a leading `/` opens a regex; in the `op` goal it divides.
  (lex-goal expr slash: RegexLiteral)
  (lex-goal op   slash: Div)

  (extras
    (trivia Space))

  (rule Source
    (seq (repeat Stmt) EOF))

  (rule Stmt
    (seq Ident Eq Expr Semi))

  (rule Expr
    (seq Primary (repeat BinOp)))

  ; Read the first value in the `expr` goal: a leading `/` is a RegexLiteral,
  ; anything else falls through to an identifier.
  (rule Primary
    (goal expr (choice slash Ident)))

  ; A trailing `/ value` is read in the `op` goal, so the `/` is division.
  (rule BinOp
    (seq (goal op slash) Ident)))
