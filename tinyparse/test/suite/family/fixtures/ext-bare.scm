; ext-bare.scm — an ILLEGAL bare redefinition of a base rule.
;
; This extension redefines the base rule Item WITHOUT wrapping it in
; (override …). The collision rule for (extends …) makes that a compile error:
; redefining a base name is allowed only through (override …), so a typo cannot
; silently shadow a base rule. Compiling this grammar must fail with a diagnostic
; that names `override`.

(pocketparse version: "0.1")

(module family.ext-bare)

(extends base)

(grammar ext-bare
  start: Source

  (rule Item
    (field word: Word)))
