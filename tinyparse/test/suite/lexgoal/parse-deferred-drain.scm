(pocketparse version: "0.1")
(grammar drain
  start: Program
  (token Space (regex "[ \t]+"))
  (token W (regex "[a-z]+"))
  (token NL "\n")
  (extras (trivia Space))
  (rule Program (seq Command EOF))
  ; Command captures TWO words into slot s, then drains s by invoking Body
  ; once per captured item. Each Body consumes one line of input. The two
  ; Body nodes are appended as siblings under Command (the enclosing rule),
  ; in FIFO order.
  (rule Command
    (seq
      (deferred-slot s)
      (field head: W)
      (defer s W)
      (defer s W)
      NL
      (parse-deferred s Body)))
  (rule Body (seq (field line: W) NL)))
