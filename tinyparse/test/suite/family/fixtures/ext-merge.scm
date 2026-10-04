; ext-merge.scm — extends base.scm and adds a NEW rule.
;
; (extends base) merges base.scm in the way (import …) already merges: its Word
; token, Space trivia, and Source/Item rules are all in scope here. This
; extension introduces a fresh rule name (Greeting) that wraps the base Item, so
; it is an additive merge — no name the base owns is redefined, so no (override
; …) is needed. The new start: rule points at the added rule.

(pocketparse version: "0.1")

(module family.ext-merge)

(extends base)

(grammar ext-merge
  start: Doc

  (rule Doc
    (seq (repeat Greeting) EOF))

  (rule Greeting
    (field hello: Item)))
