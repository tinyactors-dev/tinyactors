; word.scm — a query over base.scm: capture each word token through its field.
; Matches the Item rule's `word:` field-bound Word child and captures it as @w.
(Item word: (Word) @w)
