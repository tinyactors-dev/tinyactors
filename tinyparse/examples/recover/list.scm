; list.scm — a parenthesized-list grammar that recovers from a missing `)`.
;
; The List rule wraps its body in (recover ... (on eof ...)): when the input
; runs out before the closing parenthesis, the parser inserts a zero-width
; MISSING `)` node and raises an "unterminated list" diagnostic instead of
; aborting, so a tree is still produced. Driven by the recover/insert-eof test.

(pocketparse version: "0.1")

(module examples.recover)

(grammar list
  start: Source

  (token Space (regex "[ \t\r\n]+"))
  (token Atom  (regex "[^() \t\r\n]+"))

  (extras
    (trivia Space))

  (rule Source
    (seq (repeat Item) EOF))

  (rule Item
    (choice Atom List))

  (rule List
    (recover
      (seq
        (field open: "(")
        (repeat Item)
        (field close: ")"))
      (on eof
        message: "unterminated list"
        (label here: "expected `)` before end of file")
        help: "add a closing `)`"
        insert: ")"))))
