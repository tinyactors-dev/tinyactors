(tinyparse version: "0.1")
; Rule-level NOT: (not GUARD BODY) where BODY is a RULE, not the keyword `any'.
; The repeat consumes Item rules until the GUARD token (End = "end") is next,
; then stops -- leaving "end" for the following `tail: End'. This exercises the
; TP_VM_NOT_RULE lowering/dispatch (token-level guard lookahead, then run the
; rule body only when the guard is absent). extras: none so the guard's raw-byte
; lookahead sees the byte at the cursor.
;
; The pin is NON-VACUOUS: Item and the End word share the lexical shape
; "[a-z]+\n", so an Item could swallow the "end\n" line. The guard is the only
; thing that stops the repeat at the right spot -- if NOT_RULE always proceeds,
; Item eats "end\n", the trailing line is consumed, and `tail: End' then fails
; on EOF (parse error); if it always fails, no Item is ever produced.
(grammar notrule
  start: Program
  (token Word (regex "[a-z]+"))
  (token NL   "\n")
  (token End  "end")
  (rule Program (seq Items EOF))
  (rule Items extras: none
    (seq (repeat (not End Item))
         (field tail: End)
         NL))
  (rule Item (seq (field w: Word) NL)))
