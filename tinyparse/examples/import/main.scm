; main.scm — the importing grammar: a run of words separated by whitespace.
;
; It (import words) to pull in the Word token and the whitespace trivia defined
; in words.scm, then references Word by name. The CLI resolves `words` to
; words.scm in this directory, compiles it, and merges its definitions in before
; this grammar's body is compiled — so Word is defined by the time Source uses
; it. Imports resolve entirely at compile time; the compiled grammar and its
; packed form are exactly as if Word had been written here directly.

(pocketparse version: "0.1")

(module examples.import.main)

(import words)

(grammar main
  start: Source

  (rule Source
    (seq (repeat Word) EOF)))
