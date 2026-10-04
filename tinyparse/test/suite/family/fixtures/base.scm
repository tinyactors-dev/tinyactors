; base.scm — a tiny base grammar for the Phase 6 grammar-family tests.
;
; It defines a Word token, whitespace trivia, and a two-rule grammar that parses
; a run of words. Extensions (extends-*.scm) pull this in by name, reference its
; nodes, add rules, and override rules. The package fixture and the node-types
; test also read it.

(pocketparse version: "0.1")

(module family.base)

(grammar base
  start: Source

  (token Word  (regex "[A-Za-z]+"))
  (token Space (regex "[ \t\r\n]+"))

  (extras
    (trivia Space))

  (rule Source
    (seq (repeat Item) EOF))

  (rule Item
    (field word: Word)))
