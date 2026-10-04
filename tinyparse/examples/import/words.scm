; words.scm — a reusable module: a Word token and whitespace trivia.
;
; This file is imported by main.scm via (import words). The compiler asks the
; host's module resolver for these bytes, compiles them, and merges the Word
; token and the Space extras into the importing grammar so it can name Word
; without redefining it. A module's own start: rule (if any) is ignored when it
; is merged; only its tokens, rules, and extras carry over.

(pocketparse version: "0.1")

(module examples.import.words)

(grammar words
  (token Word  (regex "[A-Za-z]+"))
  (token Space (regex "[ \t\r\n]+"))

  (extras
    (trivia Space)))
