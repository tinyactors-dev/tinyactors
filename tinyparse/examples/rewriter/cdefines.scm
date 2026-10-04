; cdefines.scm — a line-oriented grammar for the #define -> enum rewriter.
;
; The rewriter does not need to understand C. It only needs to find
; object-like integer macros
;
;     #define NAME 123
;
; and tell them apart from every other line, which is copied through
; untouched. So the grammar models the input as a sequence of lines. A
; line is either a Define (a "#define NAME INTEGER" macro) or a Raw line
; (anything else, taken verbatim up to the newline).
;
; Whitespace is NOT declared as trivia: the rewriter reconstructs its
; output from the byte spans of the original source, so every byte —
; including the spaces and newlines it does not interpret — must remain
; reachable through the tree. Horizontal whitespace inside a #define is
; matched explicitly by the Gap token.

(tinyparse version: "0.1")

(module examples.rewriter)

(grammar cdefines
  start: Source

  ; A run of spaces and tabs (no newline). Used between the pieces of a
  ; #define line.
  (token Gap (regex "[ \t]+"))

  ; The "#define" keyword. A literal token.
  (token Hash (regex "#[ \t]*define"))

  ; A macro name: a C identifier.
  (token Name (regex "[A-Za-z_][A-Za-z0-9_]*"))

  ; An integer macro value: one or more decimal digits.
  (token Integer (regex "[0-9]+"))

  ; A single line terminator.
  (token Newline (regex "[\n]"))

  ; Any run of bytes that is neither a newline nor empty: the body of a
  ; raw, pass-through line.
  (token RawText (regex "[^\n]+"))

  (rule Source
    (seq (repeat Line) EOF))

  ; A line is a #define macro or a raw line, each ending at a newline
  ; (or end of input for the final line).
  (rule Line
    (choice Define Raw))

  ; #define NAME INTEGER, with its name and value tagged as fields so the
  ; rewriter can pull them straight out of the tree.
  (rule Define
    (seq
      Hash
      Gap
      (field name: Name)
      Gap
      (field value: Integer)
      (optional Gap)
      (choice Newline EOF)))

  ; Any other line: its bytes (which the rewriter copies verbatim),
  ; followed by a newline or end of input. An empty line is just the
  ; newline.
  (rule Raw
    (choice
      (seq RawText (choice Newline EOF))
      Newline)))
