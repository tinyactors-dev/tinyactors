; backtrack.scm -- a grammar whose ordered choice re-parses a large prefix,
; for gate:vm-diff. A Line tries List followed by "!", then "?", then ";",
; then a bare List: each alternative re-parses the same List at the same
; offset. With the rule memo forced on (TINYPARSE_RULEMEMO=always) the first
; attempt marks the key seen, the second snapshots the subtree and the third
; replays it, so the memo's success-replay path runs. No grammar of the
; standard library reaches that path on the suite's corpora.
;
; A Slashed line tries the rule Division at one offset under two lexical
; goals: in `re` its `/` scans as a Regex, in `op` as a Div. On `/ a .` the
; first attempt fails (no closing `/`) and the memo caches the failure; a memo
; whose key left out the goal would replay that failure in `op` too.
;
; A Nest line backtracks exponentially: each level tries its inner Nest
; three times, so 11 levels make about 177,000 calls, past the threshold at
; which the adaptive memo (the default) wakes in the middle of the parse.

(tinyparse version: "0.1")

(module test.backtrack)

(grammar backtrack
  start: Source

  (token Space   (regex "[ \t\r\n]+"))
  (token Comment (seq "#" (repeat (not "\n" any))))
  (token Atom    (regex "[a-z0-9]+"))
  (token slash   "/")
  (token Regex   (regex "/[^/ \n]*/"))
  (token Div     "/")

  (lex-goal re slash: Regex)
  (lex-goal op slash: Div)

  (extras
    (trivia Space)
    (trivia Comment))

  (rule Source
    (seq (repeat Line) EOF))

  (rule Line
    (choice
      (seq (field body: List) (field mark: "!"))
      (seq (field body: List) (field mark: "?"))
      (seq (field body: List) (field mark: ";"))
      List
      Slashed
      Nest))

  (rule Nest
    (choice
      (seq "<" Nest ">" "!")
      (seq "<" Nest ">" "?")
      (seq "<" (optional Nest) ">")))

  (rule Slashed
    (choice
      (seq (goal re Division) "=")
      (seq (goal op Division) ".")))

  (rule Division
    (seq slash (optional Atom)))

  (rule List
    (seq
      (field open: "(")
      (repeat Item)
      (field close: ")")))

  (rule Item
    (choice Atom List)))
