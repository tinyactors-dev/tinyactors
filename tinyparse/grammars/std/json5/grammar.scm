; grammar.scm -- std.json5, the JSON5 relaxations of JSON.
;
; JSON5 loosens strict JSON in several reader-friendly ways. This grammar adds
; the two the conformance suite exercises, building on std.jsonc so comments come
; for free:
;
;   * unquoted identifier keys -- `{a:1}', not just `{"a":1}';
;   * trailing commas in objects -- `{a:1,}'.
;
; Both touch only the object/member shape, so the grammar overrides exactly two
; base rules (Object, Member) and adds one token (Identifier). Everything else --
; strings, numbers, arrays, the keywords, and the comment trivia -- is inherited
; unchanged through (extends std.jsonc). This is OUR grammar, the third member of
; the std.json family, parameterized off the base to prove composition.

(tinyparse version: "0.1")

(module std.json5)

(extends std.jsonc)

(grammar json5
  start: Document

  ; An unquoted object key. A pragmatic ECMAScript-style identifier: a letter,
  ; underscore, or dollar to start, then any run of letters, digits, underscores,
  ; or dollars. Built from regex classes because the regex subset has no `?' or
  ; grouping; the optional tail is its own (repeat ...) piece.
  (token Identifier
    (seq (regex "[A-Za-z_$]")
         (repeat (regex "[A-Za-z0-9_$]"))))

  ; A member key may now be a quoted String (as in strict JSON) or a bare
  ; Identifier. The base Member is wrapped in (override ...) because it redefines
  ; a name std.json already defines.
  (override (rule Member
    (seq (field key: (choice String Identifier))
         ":"
         (field value: Value))))

  ; An object may end with a trailing comma: after the last member, an optional
  ; "," before the closing brace. Empty objects are still allowed (the whole
  ; member list is optional).
  (override (rule Object
    (seq (field open: "{")
         (optional
           (seq Member
                (repeat (seq "," Member))
                (optional ",")))
         (field close: "}")))))
