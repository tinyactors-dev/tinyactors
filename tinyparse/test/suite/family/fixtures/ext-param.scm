; ext-param.scm — gates a production on the `feature` parameter.
;
; This extension overrides the base Source rule so that, ONLY when the grammar is
; compiled with the `feature` parameter set, a trailing Bang ("!") is part of the
; sentence. With `--param feature` the input "hi !" parses; without it the "!" is
; not in the language and the parse fails. The (when feature: …) form includes
; its body only when the parameter is truthy. Because Source is a name the base
; already defines, the redefinition is wrapped in (override …).

(pocketparse version: "0.1")

(module family.ext-param)

(extends base)

(grammar ext-param
  start: Source

  (token Bang (regex "!"))

  (override (rule Source
    (seq (repeat Item) (when feature: Bang) EOF))))
