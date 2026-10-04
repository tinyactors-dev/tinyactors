(pocketparse version: "0.1")
(grammar rbrep
  start: Program
  (token Space (regex "[ \t]+"))
  (token W (regex "[a-z]+"))
  (token X "X")
  (token NL "\n")
  (extras (trivia Space))
  (rule Program (seq Command EOF))
  ; Each repeat iteration captures a word into slot s via (defer s W) then
  ; requires the literal "X". The repeat runs until an iteration fails. The
  ; FAILED final iteration -- the one whose word has no trailing "X" -- captured
  ; a word before failing; that capture MUST NOT leak. So the drain count equals
  ; the number of SUCCESSFUL iterations, never one more.
  ; Trailing lines follow. The two successful iterations drain into two Body
  ; nodes; the remaining trailing line is mopped up as an Extra. A leaked item
  ; from the FAILED final iteration would drain that trailing line as a third
  ; Body instead -- so the Body count (2, not 3) and Extra count (1, not 0)
  ; jointly witness that the failed iteration's defer did not leak.
  (rule Command
    (seq
      (deferred-slot s)
      (repeat (seq (defer s W) X))
      (field tail: W)
      NL
      (parse-deferred s Body)
      (repeat Extra)))
  (rule Body (seq (field line: W) NL))
  (rule Extra (seq (field extra: W) NL)))
