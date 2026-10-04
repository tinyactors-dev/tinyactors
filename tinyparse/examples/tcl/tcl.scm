; tcl.scm — a Tcl-ish grammar for tinyparse.
;
; It models Tcl commands as sequences of words, where a word is a run of
; adjacent parts: barewords, brace expansion, quoted words, variable
; substitution, command substitution, and brace groups. Two lessons live in
; this one grammar.
;
; First, recursion. Command substitution `[ … ]` and brace groups `{ … }` are
; RULES, not flat tokens: their delimiters are literals and their bodies hold
; words, which can themselves be command substitutions or brace groups. So
; `[a [b] c]` and `{a {b} c}` nest to arbitrary depth, exactly the way the
; sexpr example nests lists.
;
; Second, fused-word lexing. The pathological word
;
;     {*}"a b"${c}d[expr {1+1}]
;
; jams five substitution forms into a single word with no intervening
; whitespace. The surface forms that have no inner structure — the expansion
; marker, the quoted word, the variable reference — stay single greedy
; TOKENS, so the bytes inside a quoted word (including its space) are preserved
; verbatim and never mistaken for an inter-word separator.

(tinyparse version: "0.1")

(module examples.tcl)

(grammar tcl
  start: Source

  ; Inter-word whitespace, including line breaks. It is trivia: consumed
  ; implicitly between words and preserved in the tree, but it never appears
  ; inside a single token, so the space inside "a b" survives untouched. A
  ; trailing newline on the input is absorbed here too.
  (token Space (regex "[ \t\r\n]+"))

  ; {*} — the leading-expansion marker. A plain literal, matched before the
  ; Braces rule so it wins: `{*}` is the expansion marker, never a brace group.
  (token Expansion "{*}")

  ; "a b" — a double-quoted word. One greedy token: everything up to the
  ; closing quote, so embedded spaces are kept as part of the word.
  (token QuotedWord
    (seq "\"" (repeat (not "\"" any)) "\""))

  ; ${name} or $name — variable substitution in its two surface forms. Both
  ; are flat tokens; a variable name has no inner grammar to recurse into.
  (token VariableSub
    (choice
      (seq "${" (repeat (not "}" any)) "}")
      (seq "$" (regex "[A-Za-z0-9_]+"))))

  ; A bareword: a run of ordinary bytes that are not whitespace and not the
  ; punctuation that starts another part.
  (token Bareword
    (regex "[^ \t\r\n{}\"$[\\]]+"))

  (extras
    (trivia Space))

  (rule Source
    (seq (repeat Command) EOF))

  ; A command is one or more words. Inter-word Space is trivia, so words are
  ; separated implicitly. This small example treats a newline as ordinary
  ; whitespace rather than a command terminator.
  (rule Command
    (seq Word (repeat Word)))

  ; A word is a run of adjacent parts. Within a word there is no whitespace,
  ; but because Space is trivia it would be consumed between parts too; the
  ; tests only require the parts to appear in source order, which they do.
  (rule Word
    (seq WordPart (repeat WordPart)))

  (rule WordPart
    (choice
      Expansion
      QuotedWord
      VariableSub
      CommandSub
      Braces
      Bareword))

  ; [ … ] — command substitution. The delimiters are literal fields and the
  ; body is a script: words separated by trivia. Because a word may itself be
  ; a command substitution, `[a [b] c]` nests, and the inner `[b]` is a child
  ; CommandSub of the outer one.
  (rule CommandSub
    (seq
      (field open: "[")
      (repeat Word)
      (field close: "]")))

  ; { … } — a brace group. Same recursive shape as command substitution: the
  ; braces are literal fields and the body holds words, so `{a {b} c}` nests
  ; and `{1+1}` inside `[expr {1+1}]` is a nested brace group.
  (rule Braces
    (seq
      (field open: "{")
      (repeat Word)
      (field close: "}"))))
