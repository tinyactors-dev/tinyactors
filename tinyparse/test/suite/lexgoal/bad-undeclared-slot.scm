(pocketparse version: "0.1")
(grammar bad start: R
  (token W (regex "[a-z]+"))
  (rule R (seq (defer typo W) EOF)))
