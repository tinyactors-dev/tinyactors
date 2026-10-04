; word-eq.scm — the word query plus an #eq? predicate.
; Captures each word as @w but keeps only the match whose text equals "hi",
; so the predicate filters out the second word.
((Item word: (Word) @w) (#eq? @w "hi"))
