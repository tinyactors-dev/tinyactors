; sexpr.scm — the canonical smoke grammar: parenthesized s-expressions.
; Used by the core test suite to exercise seq/choice/repeat/field/extras.

(pocketparse version: "0.1")

(module examples.sexpr)

(grammar sexpr
  start: Source

  (token Space   (regex "[ \t\r\n]+"))
  (token Comment (seq ";" (repeat (not "\n" any))))
  (token Atom    (regex "[^() \t\r\n;]+"))

  (extras
    (trivia Space)
    (trivia Comment))

  (rule Source
    (seq (repeat Form) EOF))

  (rule Form
    (choice Atom List))

  (rule List
    (seq
      (field open: "(")
      (repeat Form)
      (field close: ")"))))
