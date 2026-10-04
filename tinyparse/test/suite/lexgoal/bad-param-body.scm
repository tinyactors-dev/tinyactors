(pocketparse version: "0.1")
(grammar bad start: R
  (token W (regex "[a-z]+"))
  (token D)
  (lex-goal g D: (param d (field x: d)))
  (rule R (seq W EOF)))
