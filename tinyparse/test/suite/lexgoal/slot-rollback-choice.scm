(pocketparse version: "0.1")
(grammar rbchoice
  start: Program
  (token Space (regex "[ \t]+"))
  (token W (regex "[a-z]+"))
  (token X "X")
  (token NL "\n")
  (extras (trivia Space))
  (rule Program (seq Command EOF))
  ; The first alternative captures a word into slot s via (defer s W), then
  ; requires the literal "X". When "X" is absent the alternative FAILS and the
  ; engine backtracks to the second alternative (field a: W). The deferred item
  ; from the failed branch MUST be discarded: the drain that follows finds the
  ; slot empty and invokes Body zero times.
  ; A trailing line follows the command. When the slot is empty (the failed
  ; branch's defer was correctly rolled back) the drain emits no Body and the
  ; trailing line is mopped up as an Extra. When a leaked item survives, the
  ; drain consumes that trailing line as a Body instead -- so the Body count is
  ; a direct, observable witness of the leak (0 = rolled back, 1 = leaked).
  (rule Command
    (seq
      (deferred-slot s)
      (choice
        (seq (defer s W) X)
        (field a: W))
      NL
      (parse-deferred s Body)
      (repeat Extra)))
  (rule Body (seq (field line: W) NL))
  (rule Extra (seq (field extra: W) NL)))
