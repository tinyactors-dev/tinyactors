; heredoc.scm — examples/heredoc
;
; Minimal shell grammar demonstrating three context-capture primitives:
;   (deferred-slot NAME)       — per-invocation FIFO slot declaration
;   (defer SLOT PATTERN)       — match PATTERN, capture text, enqueue to SLOT
;   (parse-deferred SLOT RULE) — drain SLOT, invoke RULE once per item
;
; Together with (lex-goal ... key: (param NAME BODY)), these make heredoc
; syntax expressible in the grammar without special-casing in the engine:
; the closing delimiter is captured at match time and instantiated into the
; BODY pattern before the body rule is entered.
;
; HeredocBody parses its content as structured BodyCommand instances rather
; than raw byte lines. This handles $(cat <<NESTED ... NESTED) inside a
; heredoc body correctly regardless of delimiter similarity: each
; BodyCommand invocation gets its own deferred-slot instance, so inner and
; outer heredoc queues never interfere.

(tinyparse version: "0.1")
(module examples.heredoc)

(grammar heredoc
  start: Program

  ; --- tokens
  (token Space       (regex "[ \t]+"))
  (token Newline     "\n")
  (token Word        (regex "[^ \t\n<$()]+"))
  (token HeredocOp   "<<")
  (token DollarParen "$(")
  (token CloseParen  ")")

  ; Parametrized token: no static pattern.
  ; The (param delimiter ...) body is a pattern in the grammar's own language;
  ; parse-deferred instantiates it by substituting `delimiter' with a
  ; TP_OP_LITERAL of the captured bytes, then pushes the result onto the
  ; goal-table stack. On return it pops, restoring the enclosing delimiter.
  (token HeredocDelimLine)

  (lex-goal heredoc-body-mode
    HeredocDelimLine: (param delimiter (seq delimiter "\n")))

  (extras (trivia Space))

  ; --- rules

  (rule Program
    (seq (repeat CompleteCommand) EOF))

  ; A top-level complete command: command, terminating newline, then any
  ; pending heredoc bodies drained from this invocation's slot.
  (rule CompleteCommand
    (seq
      (deferred-slot heredoc-bodies)
      (field command: Pipeline)
      Newline
      (parse-deferred heredoc-bodies HeredocBody)))

  (rule Pipeline
    (seq SimpleCommand (repeat (seq "|" SimpleCommand))))

  (rule SimpleCommand
    (seq (field name: Word)
         (repeat (choice (field redirect: Redirect)
                         (field arg:      Argument)))))

  (rule Argument
    (choice CommandSubstitution Word))

  ; Redirect captures the delimiter word into this invocation's slot.
  ; (defer heredoc-bodies Word) matches exactly like (field word: Word)
  ; and additionally enqueues the matched text onto heredoc-bodies.
  (rule Redirect
    (seq HeredocOp
         (field word: (defer heredoc-bodies Word))))

  ; extras: none so Newline and leading Space are visible to BodyCommand.
  ; The (repeat Newline) after $( consumes the line break that may follow
  ; the opening delimiter (e.g. "echo $(\n  cat ...").
  ; Between BodyCommand iterations no inter-iteration trivia is consumed;
  ; each BodyCommand ends with its own explicit Newline token.
  (rule CommandSubstitution extras: none
    (seq DollarParen
         (repeat Newline)
         (repeat BodyCommand)
         CloseParen))

  ; BodyCommand: a complete command inside a heredoc body or $(...).
  ; Structurally identical to CompleteCommand.
  ; Its deferred-slot is a SEPARATE INSTANCE from any enclosing rule's
  ; slot: the inner cat <<NESTED defers into THIS slot, not the outer
  ; CompleteCommand's slot, even though both slots are named heredoc-bodies.
  ; extras: all re-enables trivia (Space) within a BodyCommand even though it
  ; is reached from the extras: none scopes of HeredocBody and
  ; CommandSubstitution — so "cat <<NESTED" tokenizes with its space as trivia.
  (rule BodyCommand extras: all
    (seq
      (deferred-slot heredoc-bodies)
      (field command: Pipeline)
      Newline
      (parse-deferred heredoc-bodies HeredocBody)))

  ; HeredocBody: content between the heredoc operator and the closing
  ; delimiter line. extras: none — Newline is a significant content byte.
  ;
  ; parse-deferred has already pushed the delimiter pattern onto
  ; heredoc-body-mode's HeredocDelimLine stack before entering here.
  ; On return, parse-deferred pops it, restoring the enclosing delimiter.
  ;
  ; Content: zero or more BodyCommands guarded by a negative lookahead
  ; that stops the repeat when the closing delimiter line is next.
  ; End:     the delimiter line matched via the parametrized HeredocDelimLine.
  (rule HeredocBody extras: none
    (goal heredoc-body-mode
      (seq
        (repeat (not HeredocDelimLine BodyCommand))
        (field end: HeredocDelimLine)))))
