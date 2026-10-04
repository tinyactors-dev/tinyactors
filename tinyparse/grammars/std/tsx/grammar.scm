; grammar.scm — std.tsx, TypeScript with JSX.
;
; std.tsx `(extends std.typescript)`, which itself extends std.javascript, so the
; whole type system and the JSX productions are already in scope. The tsx manifest
; sets jsx on, so std.typescript's `(when jsx: jsx_element)` already restores JSX
; as a primary expression. This file only has to settle the one ambiguity the
; dialect turns on its head: a leading `<` in expression position.
;
; In std.typescript a leading `<T>` opens a type assertion (`<T>value`). In tsx
; that spelling is a JSX element instead, and a cast is written `value as T`. So
; tsx overrides `expression` to DROP the type-assertion alternative, leaving a
; leading `<` to open a JSX element (reached through primary_expression). The
; `as`/`satisfies` postfix forms stay.

(pocketparse version: "0.1")

(module std.tsx)

(extends std.typescript)

(grammar tsx
  start: program

  ; Drop the `<T>value` type assertion: in tsx a leading `<` is JSX. The
  ; as/satisfies postfix chain (from std.typescript) is the cast path here.
  (hidden
    (override (rule expression
      (choice as_satisfies_expression)))
  )

  ; A JSX opening / self-closing tag may carry type arguments after the element
  ; name (`<Element<T>>`, `<Element<T> />`), the generic-component syntax. Override
  ; the JS base's tags to admit the optional type-argument list, reusing the
  ; type_arguments rule std.typescript defines.
  (override (rule jsx_opening_element extras: all
    (seq "<" (optional (seq (field name: jsx_element_name)
                            (optional (field type_arguments: type_arguments))
                            (repeat (field attribute: jsx_attribute_item))))
         ">")))

  (override (rule jsx_self_closing_element extras: all
    (seq "<" (field name: jsx_element_name)
         (optional (field type_arguments: type_arguments))
         (repeat (field attribute: jsx_attribute_item))
         "/>"))))
