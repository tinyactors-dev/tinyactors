(tinyparse version: "0.1")
(grammar deferx
  start: Program
  (token Space (regex "[ \t]+"))
  (token W (regex "[a-z]+"))
  (extras (trivia Space))
  (rule Program (seq (repeat Item) EOF))
  (rule Item (seq (deferred-slot s) (field word: (defer s W)))))
