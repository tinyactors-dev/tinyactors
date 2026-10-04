; ident.scm — identifiers built from host CLASS bindings.
;
; The (class NAME) token primitive matches one input byte iff the host has
; installed a CLASS binding named NAME that accepts it. The pocketparse CLI
; installs four built-ins before it parses — alpha (A-Za-z), digit (0-9),
; alnum (alpha|digit), and space (space/tab/newline/cr) — so this grammar names
; them directly without writing its own byte classes.
;
; An identifier starts with an alphabetic byte and continues with alphanumerics:
;   Ident = alpha alnum*
; Whitespace is trivia between items, and Source is a run of identifiers.

(pocketparse version: "0.1")

(module examples.bindings.ident)

(grammar ident
  start: Source

  (token Space (regex "[ \t\r\n]+"))

  (token Ident
    (seq
      (class alpha)
      (repeat (class alnum))))

  (extras
    (trivia Space))

  (rule Source
    (seq (repeat Item) EOF))

  (rule Item
    Ident))
