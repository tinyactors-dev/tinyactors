(pocketparse version: "0.1")
(grammar rbopt
  start: Program
  (token Space (regex "[ \t]+"))
  (token W (regex "[a-z]+"))
  (token X "X")
  (token NL "\n")
  (extras (trivia Space))
  (rule Program (seq Command EOF))
  ; The optional body captures a word into slot s via (defer s W), then
  ; requires the literal "X". With "X" absent the optional body FAILS; the
  ; optional then succeeds (matching nothing) but the deferred item MUST be
  ; discarded, so the drain invokes Body zero times. With "X" present the
  ; optional body commits and the drain invokes Body once.
  ; The optional body captures a word via (defer s W) then requires "X". With
  ; "X" absent the body fails AFTER the defer captured -- so the rollback is the
  ; only thing keeping the slot empty. The same word is then re-consumed by
  ; (field a: W). A trailing line follows: when the defer is correctly rolled
  ; back the drain emits no Body and the trailing line becomes an Extra; a leaked
  ; item would drain it as a Body instead. (Body 0 / Extra 1 = rolled back.)
  (rule Command
    (seq
      (deferred-slot s)
      (optional (seq (defer s W) X))
      (field a: W)
      NL
      (parse-deferred s Body)
      (repeat Extra)))
  (rule Body (seq (field line: W) NL))
  (rule Extra (seq (field extra: W) NL)))
