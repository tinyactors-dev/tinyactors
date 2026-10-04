; grammar.scm — std.typescript, the JavaScript family with the TypeScript type
; system layered on.
;
; Ported (the LANGUAGE, not the code) from the build-time reference under
; tools/pocketparse/reference/tree-sitter-typescript/ — common/define-grammar.js
; (the shared type grammar) plus the JavaScript base this grammar `(extends)`.
; Node names follow tree-sitter where reasonable (type_annotation, union_type,
; interface_declaration, type_alias_declaration, generic_type, ...).
;
; The grammar `(extends std.javascript)`: every JavaScript token and rule is in
; scope, and this file ADDS the type system and OVERRIDES the handful of JS rules
; the type system has to reach into — the expression ladder (to admit `as`,
; `satisfies`, `<T>` casts and instantiation, and `!` non-null), the binding and
; parameter forms (to admit `: T` annotations), the class body (to admit member
; signatures and accessibility modifiers), and the declaration set (to admit
; interface / type / enum / namespace / declare).
;
; tree-sitter resolves the type system's ambiguities with a GLR conflict table;
; pocketparse resolves them inside the grammar with ordered choice — the type
; forms are tried in a deliberate order, and an expression-vs-type decision is
; made by which keyword or bracket leads. The `<T>`-cast-versus-JSX-element
; ambiguity is settled by the `jsx` parameter: with jsx off (std.typescript) a
; leading `<T>` opens a type assertion or a type-argument list; with jsx on
; (std.tsx) it opens a JSX element and a cast is written `x as T`.

(pocketparse version: "0.1")

(module std.typescript)

(extends std.javascript)

(grammar typescript
  start: program

  ; ------------------------------------------------------------------ tokens
  ; TypeScript keyword spellings the type grammar leads with. They are written as
  ; literal strings inline in the rules below (the engine matches a literal token
  ; by its bytes), so no new token declarations are strictly required; a handful
  ; are declared here only where a contextual word also needs to lex as a keyword.

  ; ============================================================ expressions
  ; The expression entry point. TypeScript adds `as`, `satisfies`, the `<T>` type
  ; assertion, instantiation expressions, and internal modules used as values.
  ; The JS base's `expression` was just `(choice assignment_expression)`; this
  ; override threads the TS forms in. `as`/`satisfies` are postfix over a normal
  ; assignment expression (left-associative chains: `x as A as B`).
  (hidden
    (override (rule expression
      (choice
        type_assertion
        as_satisfies_expression)))
  )

  ; `x`, `x as T`, `x as const`, `x satisfies T`, chained. The head is an ordinary
  ; assignment expression; each `as`/`satisfies` tail wraps it.
  (hidden
    (rule as_satisfies_expression
      (seq (field expression: assignment_expression)
           (repeat (choice
                     (seq "as" (field type: (choice "const" type)))
                     (seq "satisfies" (field type: type))))))
  )

  ; A `<T>expr` type assertion. This is a typescript-dialect form only: in tsx a
  ; leading `<` opens a JSX element instead. The rule is defined here and listed
  ; first in `expression` above, so std.typescript reads `<T>value` as a cast; the
  ; std.tsx grammar OVERRIDES `expression` to drop this alternative, leaving a
  ; leading `<` to open a JSX element through primary_expression. Splitting the
  ; choice across the two dialects this way needs no parameter on the rule itself.
  (rule type_assertion
    (seq (field type_arguments: type_arguments)
         (field expression: expression)))

  ; primary_expression: re-list the JS base's primary forms, dropping the
  ; unconditional jsx_element and re-adding it behind (when jsx: …) so std.tsx
  ; (jsx on) keeps JSX as a primary and std.typescript (jsx off) does not, leaving
  ; a leading `<` free to open a type assertion / type-argument list.
  (hidden
    (override (rule primary_expression
      (goal expr
        (choice
          slash
          number
          string
          template_string
          private_property_identifier
          function_expression
          class_expression
          kw_true
          kw_false
          kw_null
          kw_undefined
          kw_this
          kw_super
          meta_property
          import_expression
          (when jsx: jsx_element)
          array
          object
          parenthesized_expression
          identifier))))
  )

  ; A non-null assertion `expr!` and a `<T>(...)` instantiation/call ride on the
  ; member/call chain. Extend the JS call_tail with the TS links: a postfix `!`,
  ; and optional `<type-args>` before a call's arguments or template.
  (override (rule call_tail
    (choice
      (seq "?." (field property: member_property))
      (seq "?." "[" (field index: expression_sequence) "]")
      (seq "?." (optional (field type_arguments: type_arguments)) (field arguments: arguments))
      (seq "." (field property: member_property))
      (seq "[" (field index: expression_sequence) "]")
      "!"
      (seq (field type_arguments: type_arguments) (field arguments: arguments))
      (field type_arguments: type_arguments_instantiation)
      (field arguments: arguments)
      (field template: template_string))))

  ; A non-null assertion `expr!` can also follow an update expression (`foo++!`),
  ; which the call-chain `!` cannot reach (the `++` is applied above the chain).
  ; Override update_expression to admit a trailing run of `!`.
  (hidden
    (override (rule update_expression
      (choice
        (seq (field operator: (choice "++" "--")) (field argument: unary_expression))
        (seq (field argument: postfix_expression)
             (optional (field operator: (choice "++" "--")))
             (repeat "!")))))
  )

  ; A standalone `<T>` after a value, with nothing following — an instantiation
  ; expression `f<T>`. Distinguished from a type-assertion (which leads) and a
  ; generic call (which is followed by `(`). It must be a real type-argument list,
  ; so it reuses type_arguments.
  (rule type_arguments_instantiation (field args: type_arguments))

  ; A parenthesized expression may carry a type annotation on a single
  ; expression — `(document: any).selection` — or hold a comma sequence.
  (override (rule parenthesized_expression
    (seq "("
         (choice
           (seq expression (optional (field type: type_annotation)))
           expression_sequence)
         ")")))

  ; new with optional type arguments: `new C<T>(...)`.
  (override (rule new_expression
    (seq "new" (field constructor: call_head)
         (optional (field type_arguments: type_arguments))
         (optional (field arguments: arguments)))))

  ; ============================================================ declarations
  ; Bindings carry an optional `: T` annotation and a definite-assignment `!`.
  (override (rule variable_declarator
    (seq (field name: binding_target)
         (optional "!")
         (optional (field type: type_annotation))
         (optional (seq "=" (field value: assignment_expression))))))

  ; declaration set: the JS forms plus the TypeScript declarations.
  ;
  ; `function_declaration` MUST precede `function_signature`. The two share a
  ; head — `[async] function name call_signature` — and differ only in the tail:
  ; a declaration carries a `{ … }` body, a signature is terminated by `semi`
  ; (which is `(optional ";")`, so it can also end by ASI). Under ordered choice
  ; the body-bearing form has to be tried first; if it were second, the signature
  ; would match the head, accept an empty `semi`, and leave the body to be
  ; mis-parsed as a free-standing `statement_block` statement. So we try for a
  ; body, and only a genuinely body-less head (an overload or an ambient
  ; function) falls through to `function_signature`.
  (hidden
    (override (rule declaration
      (choice
        function_declaration
        function_signature
        abstract_class_declaration
        class_declaration
        lexical_declaration
        variable_declaration
        type_alias_declaration
        enum_declaration
        interface_declaration
        import_alias
        module_declaration
        internal_module
        ambient_declaration)))
  )

  ; type T = ...
  (rule type_alias_declaration
    (seq "type" (field name: identifier)
         (optional (field type_parameters: type_parameters))
         "=" (field value: type) semi))

  ; [const] enum Name { ... }
  (rule enum_declaration
    (seq (optional "const") "enum" (field name: identifier)
         (field body: enum_body)))

  (rule enum_body
    (seq "{"
         (optional (seq enum_member (repeat (seq "," (optional enum_member)))))
         "}"))

  (rule enum_member
    (seq (field name: property_key)
         (optional (seq "=" (field value: assignment_expression)))))

  ; interface Name<T> extends A, B { body }
  (rule interface_declaration
    (seq "interface" (field name: identifier)
         (optional (field type_parameters: type_parameters))
         (optional extends_type_clause)
         (field body: object_type)))

  (rule extends_type_clause
    (seq "extends"
         (field type: extends_type)
         (repeat (seq "," (field type: extends_type)))))

  (rule extends_type (choice generic_type nested_type_identifier identifier))

  ; namespace / module Name { ... }   (internal modules)
  (rule internal_module
    (seq "namespace" module_name (optional (field body: statement_block))))

  (rule module_declaration
    (seq "module" module_name (optional (field body: statement_block))))

  (rule module_name (choice string nested_identifier identifier))

  (rule nested_identifier
    (seq identifier (repeat (seq "." identifier_name))))

  ; import X = require("...")   and   import X = A.B
  (rule import_alias
    (seq "import" (field name: identifier) "="
         (choice import_require nested_identifier identifier) semi))

  (rule import_require
    (seq "require" "(" (field source: string) ")"))

  ; declare ...  (ambient)
  (rule ambient_declaration
    (seq "declare"
         (choice
           (seq "global" (field body: statement_block))
           (seq "module" "." (field property: identifier_name) ":" (field type: type) semi)
           declaration)))

  ; A function signature (no body), e.g. an overload or an ambient function.
  (rule function_signature
    (seq (optional "async") "function" (field name: identifier)
         call_signature semi))

  ; ============================================================ functions
  ; Function/arrow/method heads gain type parameters, parameter type annotations,
  ; and a return-type annotation. The JS forms are overridden to add them.
  (override (rule function_declaration
    (seq (optional "async") "function" (optional "*")
         (field name: identifier)
         (optional (field type_parameters: type_parameters))
         (field parameters: formal_parameters)
         (optional (field return_type: return_type_annotation))
         (field body: statement_block))))

  (override (rule function_expression
    (seq (optional "async") "function" (optional "*")
         (optional (field name: identifier))
         (optional (field type_parameters: type_parameters))
         (field parameters: formal_parameters)
         (optional (field return_type: return_type_annotation))
         (field body: statement_block))))

  ; A parameter: accessibility/readonly modifiers (parameter properties), a
  ; binding target, optional `?`, optional `: T`, optional `= default`. Rest
  ; params too.
  (override (rule formal_parameter
    (choice
      rest_parameter
      (seq (repeat decorator)
           (optional accessibility_modifier)
           (optional "override")
           (optional "readonly")
           (field pattern: (choice binding_target kw_this))
           (optional "?")
           (optional (field type: type_annotation))
           (optional (seq "=" (field value: assignment_expression)))))))

  (rule rest_parameter
    (seq "..." binding_target (optional (field type: type_annotation))))

  (rule accessibility_modifier (choice "public" "private" "protected"))

  ; Arrow functions gain type parameters, parameter type annotations, and a
  ; return type. The async-prefixed forms still come first.
  ;
  ; LEFT-FACTORED (W14-1): the `async` keyword is parsed once and the parenthesized
  ; head (type params + formal_parameters + return type) and the bare-identifier
  ; head fold into one inner choice, instead of re-matching `async` for the
  ; identifier form. The parenthesized head leads with `(` or `<` and the
  ; identifier head with a name, so the inner arms have disjoint first tokens and
  ; neither re-parses the other. `async => …` (parameter named `async`) still
  ; parses: the async branch finds no `=>` after the keyword and the choice falls
  ; through to the non-async branch.
  (override (rule arrow_function
    (choice
      (seq "async"
           (choice
             (seq (optional (field type_parameters: type_parameters))
                  (field parameters: formal_parameters)
                  (optional (field return_type: return_type_annotation)))
             (field parameters: identifier))
           "=>" arrow_body)
      (seq
           (choice
             (seq (optional (field type_parameters: type_parameters))
                  (field parameters: formal_parameters)
                  (optional (field return_type: return_type_annotation)))
             (field parameters: identifier))
           "=>" arrow_body))))

  ; catch (e: T) { ... }
  (override (rule catch_clause
    (seq "catch"
         (optional (seq "(" (field parameter: binding_target)
                        (optional (field type: type_annotation)) ")"))
         (field body: statement_block))))

  ; ============================================================ classes
  (override (rule class_declaration
    (seq (repeat decorator) "class" (field name: identifier)
         (optional (field type_parameters: type_parameters))
         (optional class_heritage) (field body: class_body))))

  (override (rule class_expression
    (seq (repeat decorator) "class" (optional (field name: identifier))
         (optional (field type_parameters: type_parameters))
         (optional class_heritage) (field body: class_body))))

  (rule abstract_class_declaration
    (seq (repeat decorator) "abstract" "class" (field name: identifier)
         (optional (field type_parameters: type_parameters))
         (optional class_heritage) (field body: class_body)))

  ; class heritage: extends <expr>[<T>] and/or implements <type>, ...
  (override (rule class_heritage
    (choice
      (seq extends_clause (optional implements_clause))
      implements_clause)))

  ; A class `extends` clause may list several heritage expressions (the
  ; flow-style `class F extends G, H<T>`), each with optional type arguments.
  (rule extends_clause
    (seq "extends" extends_clause_single
         (repeat (seq "," extends_clause_single))))

  (rule extends_clause_single
    (seq (field value: expression)
         (optional (field type_arguments: type_arguments))))

  (rule implements_clause
    (seq "implements" (field type: type) (repeat (seq "," (field type: type)))))

  ; The class body admits member signatures (no body), index signatures, and
  ; accessibility / abstract / readonly modifiers on members.
  (override (rule class_body
    (seq "{"
         (repeat (choice
                   ";"
                   class_static_block
                   index_signature_member
                   method_definition
                   field_definition))
         "}")))

  (rule index_signature_member (seq index_signature class_member_terminator))

  ; A class member terminator: an explicit `;`, a flow-style `,` (ambient class
  ; members can be comma-separated), or nothing at all (ASI at a newline).
  (rule class_member_terminator (optional (choice ";" ",")))

  ; A method definition or signature. The TS form adds modifiers, type params, a
  ; `?`, parameter and return types, and an optional body (a body-less form is a
  ; method signature / overload).
  (override (rule method_definition
    (seq (repeat decorator)
         (optional accessibility_modifier)
         (optional "static")
         (optional "override")
         (optional "readonly")
         (optional "abstract")
         method_head
         (optional "?")
         (optional (field type_parameters: type_parameters))
         (field parameters: formal_parameters)
         (optional (field return_type: return_type_annotation))
         (choice (field body: statement_block) class_member_terminator))))

  ; A field/property definition or signature, with modifiers and a `: T`.
  (override (rule field_definition
    (seq (repeat decorator)
         (optional (choice
                     (seq "declare" (optional accessibility_modifier))
                     (seq accessibility_modifier (optional "declare"))))
         (optional "static")
         (optional "override")
         (optional "abstract")
         (optional "readonly")
         (optional "accessor")
         (field property: property_key)
         (optional (choice "?" "!"))
         (optional (field type: type_annotation))
         (optional (seq "=" (field value: assignment_expression)))
         class_member_terminator)))

  ; ============================================================ export
  ; TypeScript adds `export type { … }`, `export = expr`, and
  ; `export as namespace N`. Re-list the JS export with these forms.
  ; LEFT-FACTORED (W14-1): parse the `export` keyword once and branch on what
  ; follows, instead of each alternative re-matching `export` after the prior one
  ; failed. (The `type`-led form keeps its place ahead of the bare export_clause,
  ; so `export type { … }` is not read as exporting a name `type`.)
  (override (rule export_statement
    (seq (repeat decorator)
      "export"
      (choice
        (seq "*" (optional (seq "as" import_name)) "from" (field source: string) semi)
        (seq "type" export_clause (optional (seq "from" (field source: string))) semi)
        (seq export_clause (optional (seq "from" (field source: string))) semi)
        (seq "=" expression semi)
        (seq "as" "namespace" identifier semi)
        (seq "default" (choice declaration (seq expression semi)))
        declaration))))

  ; import [type] ... ; an import specifier may carry a `type` modifier.
  (override (rule import_specifier
    (seq (optional (choice "typeof" "type"))
         (field name: import_name) (optional (seq "as" (field alias: identifier_name))))))

  (override (rule export_specifier
    (seq (optional (choice "typeof" "type"))
         (field name: import_name) (optional (seq "as" (field alias: import_name))))))

  ; import [type|typeof] <clause> from "src"  |  import "src"  |  import X = ...
  ; The `type`/`typeof` modifier is tricky: `import type from "x"` is a DEFAULT
  ; import named `type`, not a type-only import of nothing. So the modifier form
  ; is only taken when a real clause (a `{`, a `*`, or an identifier that is NOT
  ; immediately `from`) follows. The plain clause form (which admits `type` as a
  ; default name through import_clause) is tried first so `import type from "x"`
  ; reads `type` as the imported name.
  (override (rule import_statement
    (seq "import"
         (choice
           (seq import_clause "from" (field source: string))
           (seq (choice "typeof" "type") import_clause "from" (field source: string))
           (field source: string))
         (optional import_attributes)
         semi)))

  ; import_clause: a namespace import, named imports, or a default import that may
  ; be followed by `, { … }` / `, * as ns`. The default name may be `type`.
  (override (rule import_clause
    (choice
      namespace_import
      named_imports
      (seq import_default_name
           (optional (seq "," (choice namespace_import named_imports)))))))

  (rule import_default_name (choice identifier (alias identifier: "type")))

  ; A named import specifier may carry a per-name `type`/`typeof` modifier:
  ; `{ type User }`, `{ type as addUser }`, `{ addUser as type }`. The modifier
  ; is only a modifier when a further name follows; a bare `{ type }` imports the
  ; name `type`.
  (override (rule import_specifier
    (choice
      (seq (field name: import_name) "as" (field alias: identifier_name))
      (seq (choice "typeof" "type") (field name: import_name) "as" (field alias: identifier_name))
      (seq (choice "typeof" "type") (field name: import_name))
      (field name: import_name))))

  ; =================================================================== types
  ; A type annotation `: T` and a return-type annotation `: T` / `: asserts …` /
  ; `: x is T`.
  (rule type_annotation (seq ":" (field type: type)))

  (rule return_type_annotation
    (seq ":" (field type: (choice asserts type_predicate type))))

  ; A type. The ordered choice resolves the type-form ambiguities: function and
  ; constructor types (which lead with `(`/`<`/`new`) before the primary forms,
  ; conditional/union/intersection layered over the primaries.
  (rule type
    (choice
      function_type
      constructor_type
      readonly_type
      infer_type
      flow_maybe_type
      conditional_or_type))

  ; A flow-style maybe type `?T` (a leading `?` before a primary type).
  (rule flow_maybe_type (seq "?" primary_type))

  ; readonly T  (array/tuple readonly)
  (rule readonly_type (seq "readonly" type))

  ; infer X [extends T]
  (rule infer_type
    (seq "infer" identifier (optional (seq "extends" type))))

  ; A union/intersection chain, optionally completed by a conditional
  ; (`A extends B ? C : D`). union binds looser than intersection.
  (rule conditional_or_type
    (seq (field left: union_type)
         (optional (seq "extends"
                        (field right: (choice function_type flow_maybe_type union_type))
                        "?" (field consequence: type)
                        ":" (field alternative: type)))))

  ; A leading `|`/`&` is allowed (the "flow" leading-bar style).
  (rule union_type
    (seq (optional "|") intersection_type (repeat (seq "|" intersection_type))))

  (rule intersection_type
    (seq (optional "&") primary_type (repeat (seq "&" primary_type))))

  ; A primary type, with postfix `[]` (array) and `[K]` (lookup/indexed-access).
  (rule primary_type
    (seq primary_type_head
         (repeat (choice
                   (seq "[" "]")
                   (seq "[" type "]")))))

  (rule primary_type_head
    (choice
      parenthesized_type
      predefined_type
      import_type
      type_query
      index_type_query
      this_type
      existential_type
      literal_type
      template_literal_type
      object_type
      tuple_type
      generic_type
      nested_type_identifier
      type_identifier))

  (rule parenthesized_type (seq "(" type ")"))

  (rule predefined_type
    (choice "any" "number" "boolean" "string" "symbol"
            (seq "unique" "symbol")
            "void" "unknown" "never" "object"))

  (rule this_type (field this: kw_this))

  (rule existential_type (field star: "*"))

  ; A literal type: a string/number/boolean/null/undefined, or a signed number.
  (rule literal_type
    (choice
      (seq (choice "-" "+") number)
      number
      string
      kw_true
      kw_false
      kw_null
      kw_undefined))

  (rule type_identifier (field name: identifier))

  ; A.B.C — a dotted type name (at least one `.`).
  (rule nested_type_identifier
    (seq (field module: identifier)
         "." (field name: identifier_name)
         (repeat (seq "." identifier_name))))

  ; A generic type Name<T, U>.
  (rule generic_type
    (seq (field name: (choice nested_type_identifier type_identifier))
         (field type_arguments: type_arguments)))

  ; typeof X  /  keyof X
  (rule type_query (seq "typeof" type_query_operand))

  (rule type_query_operand
    (seq (choice identifier kw_this kw_import_kw)
         (repeat (choice
                   (seq "." identifier_name)
                   (seq "?." identifier_name)
                   (seq "[" (choice predefined_type string number) "]")
                   (field arguments: arguments)))
         (optional (field type_arguments: type_arguments))))

  (token kw_import_kw "import")

  (rule index_type_query (seq "keyof" primary_type))

  ; An import type `import("mod").A.B<T>` — a type that names an export of another
  ; module by the module's specifier, with an optional dotted path into it and
  ; optional type arguments. (`typeof import(...)` is the value form and is handled
  ; by `type_query` through `kw_import_kw`.) It leads with the `import` keyword, so
  ; it is unambiguous against the plain `type_identifier` that follows it here.
  (rule import_type
    (seq "import" "(" (field source: string) ")"
         (repeat (seq "." (field name: identifier_name)))
         (optional (field type_arguments: type_arguments))))

  ; An object/interface type body: a brace-delimited list of members, separated
  ; by `,` or `;` (or newline ASI), with an optional leading/trailing separator.
  ; An object/interface type body. Members are separated by `,`, `;`, or — at a
  ; newline — by nothing at all (automatic semicolon insertion), so the separator
  ; between members is optional. A leading and trailing separator are allowed.
  (rule object_type
    (seq (choice "{|" "{")
         (optional (choice "," ";"))
         (repeat (seq object_type_member (optional (choice "," ";"))))
         (choice "|}" "}")))

  (rule object_type_member
    (choice
      export_statement
      construct_signature
      call_signature
      index_signature
      method_signature
      property_signature))

  (rule property_signature
    (seq (optional accessibility_modifier)
         (optional "static")
         (optional "override")
         (optional "readonly")
         (field name: property_key)
         (optional "?")
         (optional (field type: type_annotation))))

  (rule method_signature
    (seq (optional accessibility_modifier)
         (optional "static")
         (optional "override")
         (optional "readonly")
         (optional "async")
         (optional (choice "get" "set" "*"))
         (field name: property_key)
         (optional "?")
         call_signature))

  (rule call_signature
    (seq (optional (field type_parameters: type_parameters))
         (field parameters: formal_parameters)
         (optional (field return_type: return_type_annotation))))

  (rule construct_signature
    (seq (optional "abstract") "new"
         (optional (field type_parameters: type_parameters))
         (field parameters: formal_parameters)
         (optional (field type: type_annotation))))

  ; An index signature: [k: K]: V, or a mapped-type clause [K in T]: V.
  (rule index_signature
    (seq (optional (seq (optional (choice "-" "+")) "readonly"))
         "["
         (choice
           (seq (field name: identifier) ":" (field index_type: type))
           mapped_type_clause)
         "]"
         (field type: (choice
                        type_annotation
                        (seq "-?:" type)
                        (seq "+?:" type)
                        (seq "?:" type)))))

  (rule mapped_type_clause
    (seq (field name: identifier) "in" (field type: type)
         (optional (seq "as" (field alias: type)))))

  ; A tuple type [A, B?, ...C], with labelled members.
  (rule tuple_type
    (seq "["
         (optional (seq tuple_member (repeat (seq "," tuple_member)) (optional ",")))
         "]"))

  (rule tuple_member
    (choice
      (seq "..." (field name: identifier) (field type: type_annotation))
      rest_type
      (seq (field name: identifier) "?" (field type: type_annotation))
      (seq (field name: identifier) (field type: type_annotation))
      optional_type
      type))

  (rule optional_type (seq type "?"))
  (rule rest_type (seq "..." type))

  ; A function type (T) => R, with type parameters. The return position admits a
  ; predicate (`x is T`) or an `asserts` clause as well as an ordinary type, and
  ; the order matters: `asserts` and a `type_predicate` head both also read as a
  ; plain `type` (an identifier named `asserts`, or the bare `x` of `x is T`), so
  ; under ordered choice they must be tried before `type` — otherwise `type` wins,
  ; consumes just the head, and leaves the rest of the predicate to be mis-parsed
  ; as a following statement. This mirrors the order in `return_type_annotation`.
  (rule function_type
    (seq (optional (field type_parameters: type_parameters))
         (field parameters: formal_parameters)
         "=>" (field return_type: (choice asserts type_predicate type))))

  ; A constructor type new (T) => R.
  (rule constructor_type
    (seq (optional "abstract") "new"
         (optional (field type_parameters: type_parameters))
         (field parameters: formal_parameters)
         "=>" (field type: type)))

  ; A type predicate `x is T` and an `asserts` clause.
  (rule type_predicate
    (seq (field name: (choice identifier kw_this predefined_type)) "is" (field type: type)))

  (rule asserts
    (seq "asserts" (choice type_predicate identifier kw_this)))

  ; A template literal type `a${T}b`.
  (rule template_literal_type extras: none
    (seq backtick
         (repeat (choice template_type_substitution template_chars dollar))
         "`"))

  (rule template_type_substitution extras: all
    (seq "${" (choice primary_type infer_type) "}"))

  ; Type parameters <T extends U = D, ...>.
  (rule type_parameters
    (seq "<" type_parameter (repeat (seq "," type_parameter)) (optional ",") ">"))

  (rule type_parameter
    (seq (optional "const") (field name: identifier)
         (optional (field constraint: constraint))
         (optional (seq "=" (field value: type)))))

  (rule constraint (seq (choice "extends" ":") (field type: type)))

  ; Type arguments <T, U>.
  (rule type_arguments
    (seq "<" type (repeat (seq "," type)) (optional ",") ">")))
