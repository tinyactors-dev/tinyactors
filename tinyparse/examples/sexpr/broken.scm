; broken.scm — a deliberately malformed grammar for the negative check test.
; `start:` names a rule that does not exist, and the final list is unterminated.

(tinyparse version: "0.1")

(grammar sexpr
  start: Nonexistent

  (rule Form
    (choice Atom List
