; ext-override.scm — overrides a base rule via (override …).
;
; The base Item is (field word: Word). This extension redefines Item so it
; accepts EITHER a Word or a Number, naming the new token. Because Item is a name
; the base already owns, the redefinition is wrapped in (override …); that makes
; the intent explicit and lets a digit run parse where the base grammar would
; reject it.

(pocketparse version: "0.1")

(module family.ext-override)

(extends base)

(grammar ext-override
  start: Source

  (token Number (regex "[0-9]+"))

  (override (rule Item
    (field word: (choice Word Number)))))
