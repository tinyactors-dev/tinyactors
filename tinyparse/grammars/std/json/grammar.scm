; grammar.scm -- std.json, the standard JSON grammar.
;
; A small, complete JSON grammar: objects, arrays, strings, numbers, and the
; keywords true/false/null. Whitespace is trivia (extras), so it is skipped
; between tokens. The start rule is Document: a single JSON value followed by
; end of input. This is OUR grammar; it lives under grammars/std because it is
; authored and owned here, not ported from a third party.

(tinyparse version: "0.1")

(module std.json)

(grammar json
  start: Document

  ; Insignificant whitespace between tokens: spaces, tabs, and line breaks.
  (token Space (regex "[ \t\r\n]+"))

  ; A JSON string: double-quoted, with backslash escapes. A string body byte is
  ; either an escape (a backslash followed by any byte) or any byte that is
  ; neither a quote nor a backslash (the [^"\\] class).
  (token String
    (seq "\""
         (repeat (choice (seq "\\" any) (regex "[^\"\\\\]")))
         "\""))

  ; A JSON number: an optional minus, an integer part, an optional fraction, and
  ; an optional exponent. Built from lexical combinators because the regex subset
  ; has no grouping or `?'; each optional run is its own (repeat ... 0 1) piece.
  (token Number
    (seq (optional "-")
         (regex "[0-9]+")
         (optional (seq "." (regex "[0-9]+")))
         (optional (seq (regex "[eE]")
                        (optional (regex "[+-]"))
                        (regex "[0-9]+")))))

  (token True  "true")
  (token False "false")
  (token Null  "null")

  (extras
    (trivia Space))

  ; A document is one value, then end of input.
  (rule Document
    (seq Value EOF))

  (rule Value
    (choice Object Array String Number True False Null))

  ; An object: braces around comma-separated members; empty objects allowed.
  (rule Object
    (seq (field open: "{")
         (optional
           (seq Member (repeat (seq "," Member))))
         (field close: "}")))

  (rule Member
    (seq (field key: String) ":" (field value: Value)))

  ; An array: brackets around comma-separated values; empty arrays allowed.
  (rule Array
    (seq (field open: "[")
         (optional
           (seq Value (repeat (seq "," Value))))
         (field close: "]"))))
