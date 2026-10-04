; grammar.scm — the grammar source for the `family.demo` package fixture (W29).
;
; A run of words, with a trailing Bang ("!") gated on the package's `feature`
; parameter (declared in package.scm, bound on by default). Resolving and running
; the package compiles this grammar with that parameter in scope, so "hi !"
; parses through the CLI.

(tinyparse version: "0.1")

(module family.demo)

(grammar demo
  start: Source

  (token Word  (regex "[A-Za-z]+"))
  (token Space (regex "[ \t\r\n]+"))
  (token Bang  (regex "!"))

  (extras
    (trivia Space))

  (rule Source
    (seq (repeat Item) (when feature: Bang) EOF))

  (rule Item
    (field word: Word)))
