(tinyparse version: "0.1")
(grammar bad start: R
  (token W (regex "[a-z]+"))
  (rule R (seq (deferred-slot s) (defer s W) (parse-deferred s Nope) EOF)))
