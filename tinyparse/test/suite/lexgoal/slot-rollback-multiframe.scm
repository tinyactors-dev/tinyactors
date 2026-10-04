; Slots live in TWO different CALL frames (Outer's `a`, Inner's `b`), both
; active when Inner's choice speculates. A wrong push/restore ORDER (not just a
; disabled restore) would cross-assign the saved counts between frames, so the
; drains land on the wrong slot. Correct: a keeps its item (BodyA drains), b
; rolls back to empty (BodyB never drains).
(tinyparse version: "0.1")
(grammar mf
  start: Program
  (token Space (regex "[ \t]+"))
  (token W     (regex "[a-z]+"))
  (token NL    "\n")
  (token X     "X")
  (extras (trivia Space))
  (rule Program (seq Outer EOF))
  (rule Outer (seq (deferred-slot a)
                   (defer a W)
                   Inner
                   (parse-deferred a BodyA)))
  (rule Inner extras: none
    (seq (deferred-slot b)
         (choice (seq (defer b W) X)
                 (field x: W))
         NL
         (parse-deferred b BodyB)))
  (rule BodyA extras: none (seq (field la: W) NL))
  (rule BodyB extras: none (seq (field lb: W) NL)))
