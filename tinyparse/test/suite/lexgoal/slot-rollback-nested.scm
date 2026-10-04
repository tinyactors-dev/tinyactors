(pocketparse version: "0.1")
(grammar rbnest
  start: Program
  (token Space (regex "[ \t]+"))
  (token W (regex "[a-z]+"))
  (token X "X")
  (token Y "Y")
  (token NL "\n")
  (extras (trivia Space))
  (rule Program (seq Command EOF))
  ; Nested choice. The OUTER first alternative is itself a CHOICE: its inner
  ; first alternative defers a word then needs "Y"; with "Y" absent the inner
  ; alt backtracks (inner defer rolled back) to the inner second alt, which
  ; defers another word then needs "X"; with "X" absent that fails too, so the
  ; whole outer first alt fails and the outer second alt (field a: W) matches.
  ; Both nested deferred items must be discarded -- the sentinels pop in order,
  ; no over- or under-restore -- so the drain invokes Body zero times.
  ; A trailing line follows. Both nested deferred items must be rolled back, so
  ; the drain emits no Body and the trailing line becomes an Extra. Any leaked
  ; item (over- or under-restore of the nested sentinels) would drain the
  ; trailing line as a Body -- so the Body count witnesses correct nesting.
  (rule Command
    (seq
      (deferred-slot s)
      (choice
        (choice
          (seq (defer s W) Y)
          (seq (defer s W) X))
        (field a: W))
      NL
      (parse-deferred s Body)
      (repeat Extra)))
  (rule Body (seq (field line: W) NL))
  (rule Extra (seq (field extra: W) NL)))
