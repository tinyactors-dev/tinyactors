; grammar.scm -- std.jsonc, JSON with comments.
;
; JSON-with-comments is strict JSON plus two comment forms that may appear
; anywhere whitespace may: a line comment `// ...' running to end of line, and a
; block comment `/* ... */'. Both are insignificant, so they are trivia: the
; grammar adds a Comment token to (extras (trivia ...)), where the base already
; lists Space, and they are skipped between tokens like whitespace.
;
; The whole of strict JSON -- objects, arrays, strings, numbers, the keywords --
; is pulled in by (extends std.json); nothing of the base is overridden, because
; comments are purely additive trivia. This is OUR grammar, authored and owned
; here, parameterized off std.json to prove grammar composition.

(tinyparse version: "0.1")

(module std.jsonc)

(extends std.json)

(grammar jsonc
  start: Document

  ; A comment, in either JSON-with-comments form. The regex subset has no
  ; alternation, so the two forms are an explicit (choice ...) of lexical
  ; combinators, the way the base builds String and Number.
  ;
  ;   line:  `//' then any run of bytes that are not a line break.
  ;   block: `/*' then any run of bytes up to the closing `*/', matched one byte
  ;          at a time with (not "*/" any) -- consume a byte only when `*/' does
  ;          not start here -- then the closing `*/'.
  (token Comment
    (choice
      (seq "//" (repeat (regex "[^\r\n]")))
      (seq "/*" (repeat (not "*/" any)) "*/")))

  ; Merge the new trivia with the base's: Space stays trivia (the base also
  ; lists it; extras append, they do not replace), and Comment joins it, so a
  ; comment is skipped wherever whitespace is.
  (extras
    (trivia Comment)))
