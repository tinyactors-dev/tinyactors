; grammar.scm — std.javascript, a competent core JavaScript grammar.
;
; Ported (the LANGUAGE, not the code) from the build-time reference under
; tools/pocketparse/reference/tree-sitter-javascript/. Node names follow
; tree-sitter where reasonable (program, expression_statement, binary_expression,
; call_expression, member_expression, arrow_function, ...).
;
; Two hard problems tree-sitter solves with a hand-written C scanner and a GLR
; conflict table, this grammar solves inside the grammar with primitives the
; engine already ships:
;
;   * regex-versus-division — the byte `/` opens a regex literal at the start of
;     an expression but divides after a value. A lexical goal (`expr`) redirects
;     a `slash` trigger token to a `regex` literal where a primary is expected;
;     elsewhere `/` is the division operator.
;
;   * automatic semicolon insertion — a statement terminator is OPTIONAL. Where
;     the source omits the `;`, the optional terminator simply does not match and
;     the statement still completes, so newline-terminated statements parse
;     without a host scanner.
;
; Expression precedence is a ladder of ordered choices, lowest precedence at the
; top (the pattern the `jit` example uses): sequence > assignment > ternary >
; nullish/logical-or > logical-and > bitwise > equality > relational > shift >
; additive > multiplicative > exponent > unary > update > postfix (call / member
; / subscript) > primary.

(pocketparse version: "0.1")

(module std.javascript)

(grammar javascript
  start: program

  ; ------------------------------------------------------------------ trivia
  ; Whitespace: ASCII blanks, plus the Unicode space and zero-width separators
  ; JavaScript treats as whitespace -- the byte-order mark / zero-width no-break
  ; space U+FEFF, word joiner U+2060, zero-width space U+200B, no-break space
  ; U+00A0 -- and the two Unicode line terminators U+2028 / U+2029. Each is its
  ; own UTF-8 byte sequence; the lexer is byte-oriented, so they are spelt as
  ; literal byte runs (written with \xHH escapes) rather than a character class.
  (token Space
    (repeat
      (choice
        (regex "[ \t\r\n]")
        (regex "\\xC2\\xA0")           ; U+00A0 no-break space
        (regex "\\xEF\\xBB\\xBF")      ; U+FEFF zero-width no-break space / BOM
        (regex "\\xE2\\x81\\xA0")      ; U+2060 word joiner
        (regex "\\xE2\\x80\\x8B")      ; U+200B zero-width space
        (regex "\\xE2\\x80\\xA8")      ; U+2028 line separator
        (regex "\\xE2\\x80\\xA9"))     ; U+2029 paragraph separator
      min: 1))
  ; Line comment: `//` to end of line. A line terminator ends it -- the ASCII
  ; newline, or a Unicode U+2028 / U+2029 line separator (so a `//` comment that
  ; runs up to a U+2028 stops there, as the spec requires). Block comment:
  ; `/* ... */`, built from lexical combinators because the regex subset has no
  ; alternation.
  (token line_comment
    (seq "//"
         (repeat (not (choice "\n" (regex "\\xE2\\x80\\xA8") (regex "\\xE2\\x80\\xA9"))
                      any))))
  (token block_comment
    (seq "/*"
         (repeat (choice (not "*" any)
                         (seq "*" (not "/" any))))
         (repeat "*")
         "/"))
  (token hash_bang (seq "#!" (repeat (not "\n" any))))

  ; HTML-style open comment `<!--`, a web-legacy part of the grammar: it opens a
  ; comment that runs to the end of the line, treated as trivia like a `//`
  ; comment. The close form `-->` is NOT trivia -- it would clash with `a-- > b`
  ; -- so it is a statement (html_comment_close) reached only where a statement
  ; is expected, never mid-expression.
  (token html_comment
    (seq "<!--" (repeat (not "\n" any))))
  (token html_comment_close
    (seq "-->" (repeat (not "\n" any))))

  ; Whitespace attaches on sight (the default, `attach: trailing'): it binds nowhere
  ; anyone queries, so holding it would be pure cost. The three comment kinds attach
  ; `leading': a comment is held and re-emitted at the next sibling boundary, so a
  ; doc comment lands ahead of what it documents (a sibling in the parent) instead of
  ; being swept into the node that is closing (docs/grammar.md, "Extras").
  ; std.typescript and std.tsx inherit this through (extends std.javascript).
  (extras
    (trivia Space)
    (trivia line_comment  attach: leading)
    (trivia block_comment attach: leading)
    (trivia html_comment  attach: leading))

  ; ------------------------------------------------------------------ literals
  ; An identifier character is an ASCII letter/digit/_/$, OR a `\u`-escape
  ; (`a` or `\u{62}`), OR any byte >= 0x80. The last admits UTF-8 multibyte
  ; letters (`últimaVez`, `県`): treating every continuation/lead byte as an
  ; identifier byte is coarse but enough for the corpus, and the engine's lexer
  ; is byte-oriented. `id_start` excludes the ASCII digits; `id_part` includes
  ; them. The `\x80-\xFF` range uses the `\xHH` regex escape the engine added so
  ; a class can name a high byte the .scm string syntax cannot hold directly.
  ; Identifier: an id-start then id-parts, wrapped in (reserved ...) so a keyword
  ; spelling is NOT swallowed as a plain identifier. Without the guard a reserved
  ; word such as `case` or `default` would tokenize as an identifier and the
  ; keyword-led rule that needs it (an empty fall-through `switch` arm, say) would
  ; never be reached. The reserved set is the ECMAScript reserved words;
  ; contextual words (`of`, `as`, `from`, `get`, `set`, `async`, `static`,
  ; `await`, `yield`, `let`, `undefined`) are deliberately NOT reserved, because
  ; the language also admits them as identifiers.
  ;
  ; An id-start is an ASCII letter/_/$, a `\u`-escape (`a` or `\u{62}`), or
  ; any byte >= 0x80. The high-byte range admits UTF-8 multibyte letters
  ; (`últimaVez`, `県`): treating every lead/continuation byte as an identifier
  ; byte is coarse but enough for the corpus, since the lexer is byte-oriented.
  ; id-part additionally admits ASCII digits. The patterns are inlined (a token
  ; cannot reference another token -- the lexer matches token IR without rule
  ; resolution) so id-start and id-part are spelt out at each use.
  (token identifier
    (reserved
      (seq (choice (regex "[A-Za-z_$\\x80-\\xFF]")
                   (seq "\\u{" (repeat (regex "[0-9a-fA-F]") min: 1) "}")
                   (seq "\\u" (regex "[0-9a-fA-F]") (regex "[0-9a-fA-F]")
                              (regex "[0-9a-fA-F]") (regex "[0-9a-fA-F]")))
           (repeat (choice (regex "[A-Za-z0-9_$\\x80-\\xFF]")
                           (seq "\\u{" (repeat (regex "[0-9a-fA-F]") min: 1) "}")
                           (seq "\\u" (regex "[0-9a-fA-F]") (regex "[0-9a-fA-F]")
                                      (regex "[0-9a-fA-F]") (regex "[0-9a-fA-F]")))))
      "break" "case" "catch" "class" "const" "continue" "debugger" "default"
      "delete" "do" "else" "export" "extends" "false" "finally" "for"
      "function" "if" "import" "in" "instanceof" "new" "null" "return" "super"
      "switch" "this" "throw" "try" "typeof" "var" "void" "while" "with"
      "true"))

  ; A property / member name: the same spelling as identifier but NOT reserved,
  ; because `obj.class`, `{ for: 1 }`, and `export { x as default }` are all
  ; legal -- a reserved word is a fine property or member name. Used wherever the
  ; grammar names a member, property key, or import/export name.
  (token identifier_name
    (seq (choice (regex "[A-Za-z_$\\x80-\\xFF]")
                 (seq "\\u{" (repeat (regex "[0-9a-fA-F]") min: 1) "}")
                 (seq "\\u" (regex "[0-9a-fA-F]") (regex "[0-9a-fA-F]")
                            (regex "[0-9a-fA-F]") (regex "[0-9a-fA-F]")))
         (repeat (choice (regex "[A-Za-z0-9_$\\x80-\\xFF]")
                         (seq "\\u{" (repeat (regex "[0-9a-fA-F]") min: 1) "}")
                         (seq "\\u" (regex "[0-9a-fA-F]") (regex "[0-9a-fA-F]")
                                    (regex "[0-9a-fA-F]") (regex "[0-9a-fA-F]"))))))

  (token private_property_identifier
    (seq "#" (regex "[A-Za-z_$\\x80-\\xFF]")
         (repeat (regex "[A-Za-z0-9_$\\x80-\\xFF]"))))

  ; Number. Built from combinators: the regex subset has no grouping/alternation,
  ; so each numeric shape is its own (seq ...). Order matters inside choice — the
  ; longer/more-specific forms (hex/bin/oct, then bigint `n`, then float) come
  ; before the bare decimal so a prefix is not mistaken for a plain integer.
  (token number
    (choice
      ; hex / binary / octal, optional bigint suffix
      (seq (choice "0x" "0X") (regex "[0-9a-fA-F]") (repeat (regex "[0-9a-fA-F_]")) (optional "n"))
      (seq (choice "0b" "0B") (regex "[01]")        (repeat (regex "[01_]"))        (optional "n"))
      (seq (choice "0o" "0O") (regex "[0-7]")       (repeat (regex "[0-7_]"))       (optional "n"))
      ; .5  .5e6
      (seq "." (regex "[0-9]") (repeat (regex "[0-9_]"))
           (optional (seq (regex "[eE]") (optional (regex "[+-]")) (regex "[0-9]") (repeat (regex "[0-9_]")))))
      ; 123 123n 123.45 123.45e-6 123e6
      (seq (regex "[0-9]") (repeat (regex "[0-9_]"))
           (choice
             "n"
             (seq (optional (seq "." (repeat (regex "[0-9_]"))))
                  (optional (seq (regex "[eE]") (optional (regex "[+-]")) (regex "[0-9]") (repeat (regex "[0-9_]")))))))))

  ; Strings: double- and single-quoted, with backslash escapes (any escaped byte).
  (token string
    (choice
      (seq "\"" (repeat (choice (seq "\\" any) (regex "[^\"\\\\]"))) "\"")
      (seq "'"  (repeat (choice (seq "\\" any) (regex "[^'\\\\]")))  "'")))

  ; The ambiguous `/` trigger and the two tokens a goal redirects it to. `slash`
  ; is never emitted as a node: in the `expr` goal it becomes a `regex` literal,
  ; in the `div` goal it becomes the division operator `/`.
  (token slash "/")
  (token regex
    (seq "/"
         (repeat
           (choice
             (seq "\\" (not "\n" any))
             (seq "[" (repeat (choice (seq "\\" (not "\n" any)) (regex "[^\\]\\n\\\\]"))) "]")
             (regex "[^/\\\\\n\\[]")))
         "/"
         (repeat (regex "[a-z]"))))
  (token Div "/")

  (lex-goal expr slash: regex)
  (lex-goal div  slash: Div)

  ; Template strings. The chunk between holes is scanned by `template_chars`: any
  ; run of bytes that is not a backtick, a backslash, or a `${` opener. A hole is
  ; `${ expression }`; holes nest because the body is the same expression grammar
  ; (and a template inside a hole re-enters this rule).
  (token backtick "`")
  ; A run of template text: bytes that are not a backtick, a backslash, or a `$`,
  ; plus backslash escapes. A `$` is handled in the template_string rule (it
  ; either opens a `${` substitution or stands as a literal dollar), so it is not
  ; part of this run; that keeps the run from swallowing a closing backtick.
  (token template_chars
    (repeat (choice
              (seq "\\" any)
              (regex "[^`\\\\$]"))
            min: 1))
  (token dollar "$")

  ; ----------------------------------------------------------------------- JSX
  ; JSX text: a run of child text between tags. It stops at the bytes that open
  ; another JSX construct -- `<` (a tag), `{` (an expression hole), `}`, `>` --
  ; and at `&` (a character reference, scanned as its own node). Whitespace is
  ; significant text here, so the jsx child list runs under `extras: none` and
  ; jsx_text carries the spaces verbatim.
  (token jsx_text
    (repeat (regex "[^<>{}&]") min: 1))

  ; An HTML character reference: `&name;`, `&#123;`, or `&#xAF;`. Used both in JSX
  ; child text and inside JSX attribute strings.
  (token html_character_reference
    (seq "&"
         (choice
           (seq "#" (choice "x" "X") (repeat (regex "[0-9a-fA-F]") min: 1))
           (seq "#" (repeat (regex "[0-9]") min: 1))
           (repeat (regex "[A-Za-z]") min: 1))
         ";"))

  ; JSX string fragments: the text inside a `"`/`'` attribute value, up to the
  ; quote or an `&` (which starts a character reference). Unlike a normal JS
  ; string these may contain raw newlines and do not process backslash escapes.
  (token jsx_double_string_fragment (repeat (regex "[^\"&]") min: 1))
  (token jsx_single_string_fragment (repeat (regex "[^'&]") min: 1))

  ; A JSX element/attribute identifier may contain a hyphen (`data-i8n`), which a
  ; normal JS identifier cannot.
  (token jsx_identifier
    (seq (regex "[A-Za-z_$]") (repeat (regex "[A-Za-z0-9_$\\-]"))))

  ; ------------------------------------------------------------------ keywords
  ; Reserved words. Defined as tokens so the lexer prefers them over `identifier`
  ; when the spelling matches (longest-match plus keyword priority in the engine).
  (token kw_var      "var")
  (token kw_let      "let")
  (token kw_const    "const")
  (token kw_function "function")
  (token kw_return   "return")
  (token kw_if       "if")
  (token kw_else     "else")
  (token kw_for      "for")
  (token kw_while    "while")
  (token kw_do       "do")
  (token kw_switch   "switch")
  (token kw_case     "case")
  (token kw_default  "default")
  (token kw_break    "break")
  (token kw_continue "continue")
  (token kw_throw    "throw")
  (token kw_try      "try")
  (token kw_catch    "catch")
  (token kw_finally  "finally")
  (token kw_new      "new")
  (token kw_delete   "delete")
  (token kw_typeof   "typeof")
  (token kw_void     "void")
  (token kw_instanceof "instanceof")
  (token kw_in       "in")
  (token kw_of       "of")
  (token kw_class    "class")
  (token kw_extends  "extends")
  (token kw_super    "super")
  (token kw_this     "this")
  (token kw_import   "import")
  (token kw_export   "export")
  (token kw_from     "from")
  (token kw_as       "as")
  (token kw_async    "async")
  (token kw_await    "await")
  (token kw_yield    "yield")
  (token kw_static   "static")
  (token kw_get      "get")
  (token kw_set      "set")
  (token kw_debugger "debugger")
  (token kw_with     "with")
  (token kw_true     "true")
  (token kw_false    "false")
  (token kw_null     "null")
  (token kw_undefined "undefined")

  ; =================================================================== program
  (rule program
    (seq (optional hash_bang) (repeat statement) EOF))

  ; A statement terminator. Optional `;` is how automatic semicolon insertion is
  ; expressed: where the source omits it the rule simply matches nothing.
  (rule semi (optional ";"))

  ; =============================================================== statements
  (hidden
    (rule statement
      (choice
        empty_statement
        html_comment_statement
        import_statement
        export_statement
        declaration
        statement_block
        if_statement
        switch_statement
        for_statement
        while_statement
        do_statement
        try_statement
        with_statement
        break_statement
        continue_statement
        return_statement
        throw_statement
        debugger_statement
        labeled_statement
        expression_statement))
  )

  (rule empty_statement (field semi: ";"))

  ; An HTML close comment `-->` standing as a statement. It is reported under the
  ; alias html_comment so the node name matches the web-legacy comment form.
  (rule html_comment_statement (alias html_comment: html_comment_close))

  (rule statement_block
    (seq (field open: "{") (repeat statement) (field close: "}")))

  ; A leading `{` is a block; `class`, `function`, `let`/`const`/`var` start
  ; declarations; everything else is an expression statement. Ordered choice in
  ; `statement` resolves which (declarations and blocks come before the
  ; expression_statement fallback).

  (hidden
    (rule declaration
      (choice
        function_declaration
        class_declaration
        lexical_declaration
        variable_declaration))
  )

  (rule lexical_declaration
    (seq (field kind: (choice "let" "const"))
         variable_declarator
         (repeat (seq "," variable_declarator))
         semi))

  (rule variable_declaration
    (seq (field kind: "var")
         variable_declarator
         (repeat (seq "," variable_declarator))
         semi))

  (rule variable_declarator
    (seq (field name: binding_target)
         (optional (seq "=" (field value: assignment_expression)))))

  ; A binding target: a name or a destructuring pattern.
  (rule binding_target
    (choice identifier array_pattern object_pattern))

  (rule array_pattern
    (seq "[" (optional (seq pattern_element (repeat (seq "," (optional pattern_element))))) "]"))

  (rule object_pattern
    (seq "{" (optional (seq pattern_property (repeat (seq "," (optional pattern_property))))) "}"))

  (rule pattern_element
    (choice
      rest_pattern
      (seq binding_target (optional (seq "=" (field value: assignment_expression))))))

  (rule pattern_property
    (choice
      rest_pattern
      (seq (field key: property_key) ":" (field value: pattern_element))
      (seq (field shorthand: identifier) (optional (seq "=" (field value: assignment_expression))))))

  (rule rest_pattern (seq "..." binding_target))

  (rule expression_statement
    (seq expression_sequence semi))

  (rule if_statement
    (seq "if" (field condition: parenthesized_expression)
         (field consequence: statement)
         (optional else_clause)))

  (rule else_clause (seq "else" (field alternative: statement)))

  (rule switch_statement
    (seq "switch" (field value: parenthesized_expression) (field body: switch_body)))

  (rule switch_body
    (seq "{" (repeat (choice switch_case switch_default)) "}"))

  (rule switch_case
    (seq "case" (field value: expression_sequence) ":" (repeat statement)))

  (rule switch_default
    (seq "default" ":" (repeat statement)))

  ; for ( init ; cond ; incr ) body   and   for ( decl in/of expr ) body
  (rule for_statement
    (seq "for"
         (optional "await")
         "("
         (choice for_in_of for_classic)
         (field body: statement)))

  (rule for_classic
    (seq
      (choice
        lexical_declaration_noterm
        variable_declaration_noterm
        (seq (optional expression_sequence) ";"))
      (optional expression_sequence) ";"
      (optional expression_sequence)
      ")"))

  ; for ( [kind] target in/of expr ) — the head of a for-in / for-of loop. The
  ; declaration kind may be `let`/`const`/`var`, or a `using`/`await using`
  ; binding (the explicit-resource-management forms). An optional `= init` on the
  ; left covers the quirky `for (var x = y in z)`.
  (rule for_in_of
    (seq
      (choice
        ; A declared binding: `for (let x in …)`. The kind is tried with its
        ; binding_target as one unit, so a bare `for (let in {})` -- where `let`
        ; is itself the loop variable, not a kind -- falls through to the
        ; kind-less form below instead of committing to `kind: let`. The optional
        ; `= init` after a declared binding covers `for (var foo = bar in baz)`;
        ; the initializer must not itself read the `in`, so it is a no-`in`
        ; expression.
        (seq (field kind: (choice (seq "await" "using") "using" "let" "const" "var"))
             (field left: binding_target)
             (optional (seq "=" (field init: expression_no_in))))
        ; A kind-less left: an assignment target such as `x.y`, `x[i]`, or a bare
        ; name, OR a destructuring pattern, OR a parenthesized name. member_or_call
        ; is tried before the bare binding_target so `x[i]`/`x.y` are consumed
        ; whole rather than stopping after `x`; parenthesized_expression covers
        ; `for ((a) in b)`.
        (field left: (choice array_pattern object_pattern member_or_call parenthesized_expression)))
      (field operator: (choice "in" "of"))
      (field right: expression_sequence)
      ")"))

  ; A no-`in` expression: the ternary ladder entered below relational, so the
  ; `in` relational operator is unavailable. Used for the initializer of a
  ; for-in binding, where a trailing `in` belongs to the loop header, not the
  ; initializer (`for (var foo = bar in baz)`).
  (rule expression_no_in (field value: shift_expression))

  ; for-classic declarations consume their own `;`, so they are written without
  ; the trailing semi the statement-level forms carry.
  (rule lexical_declaration_noterm
    (seq (field kind: (choice "let" "const"))
         variable_declarator (repeat (seq "," variable_declarator)) ";"))
  (rule variable_declaration_noterm
    (seq (field kind: "var")
         variable_declarator (repeat (seq "," variable_declarator)) ";"))

  (rule while_statement
    (seq "while" (field condition: parenthesized_expression) (field body: statement)))

  (rule do_statement
    (seq "do" (field body: statement) "while"
         (field condition: parenthesized_expression) semi))

  (rule try_statement
    (seq "try" (field body: statement_block)
         (optional catch_clause)
         (optional finally_clause)))

  (rule catch_clause
    (seq "catch"
         (optional (seq "(" (field parameter: binding_target) ")"))
         (field body: statement_block)))

  (rule finally_clause (seq "finally" (field body: statement_block)))

  (rule with_statement
    (seq "with" (field object: parenthesized_expression) (field body: statement)))

  (rule break_statement    (seq "break"    (optional (field label: identifier)) semi))
  (rule continue_statement (seq "continue" (optional (field label: identifier)) semi))
  (rule return_statement   (seq "return"   (optional expression_sequence) semi))
  (rule throw_statement    (seq "throw"    expression_sequence semi))
  (rule debugger_statement (seq "debugger" semi))

  ; A labeled statement: NAME : statement. Ordered after the keyword statements
  ; in `statement`, but it must not swallow them, so the label is a plain
  ; identifier followed by a colon.
  (rule labeled_statement
    (seq (field label: identifier) ":" (field body: statement)))

  ; --------------------------------------------------------------- import/export
  ; The `import` statement. A trailing import-attributes clause -- `with { type:
  ; "json" }` (or the older `assert { ... }`) -- is optional after the source.
  (rule import_statement
    (seq "import"
         (choice
           (seq import_clause "from" (field source: string))
           (field source: string))
         (optional import_attributes)
         semi))

  (rule import_attributes
    (seq (choice "with" "assert")
         "{" (optional (seq pair (repeat (seq "," (optional pair))))) "}"))

  (rule import_clause
    (choice
      namespace_import
      named_imports
      (seq identifier (optional (seq "," (choice namespace_import named_imports))))))

  (rule namespace_import (seq "*" "as" identifier_name))

  (rule named_imports
    (seq "{" (optional (seq import_specifier (repeat (seq "," (optional import_specifier))))) "}"))

  (rule import_specifier
    (seq (field name: import_name) (optional (seq "as" (field alias: identifier_name)))))

  (rule import_name (choice identifier_name string))

  ; Decorators may precede an `export class`, so the statement accepts a leading
  ; run of decorators before the `export` keyword.
  ; LEFT-FACTORED (W14-1): the `export` keyword is parsed once and the four forms
  ; branch on what follows, instead of each alternative re-matching `export` after
  ; the previous one failed. The distinguishing second token (`*`, `{`/specifier,
  ; `default`, or a declaration lead) selects the branch.
  (rule export_statement
    (seq (repeat decorator)
      "export"
      (choice
        (seq "*" (optional (seq "as" import_name)) "from" (field source: string) semi)
        (seq export_clause (optional (seq "from" (field source: string))) semi)
        (seq "default" (choice declaration (seq expression semi)))
        declaration)))

  (rule export_clause
    (seq "{" (optional (seq export_specifier (repeat (seq "," (optional export_specifier))))) "}"))

  (rule export_specifier
    (seq (field name: import_name) (optional (seq "as" (field alias: import_name)))))

  ; =============================================================== functions
  (rule function_declaration
    (seq (optional "async") "function" (optional "*")
         (field name: identifier) (field parameters: formal_parameters)
         (field body: statement_block)))

  (rule function_expression
    (seq (optional "async") "function" (optional "*")
         (optional (field name: identifier))
         (field parameters: formal_parameters)
         (field body: statement_block)))

  (rule formal_parameters
    (seq "(" (optional (seq formal_parameter (repeat (seq "," (optional formal_parameter))))) ")"))

  (rule formal_parameter
    (choice
      rest_pattern
      (seq binding_target (optional (seq "=" (field value: assignment_expression))))))

  ; Arrow functions. The head is a bare identifier or a parenthesized parameter
  ; list, optionally preceded by `async`. `async` is also a valid parameter name
  ; (`async => async + 1`).
  ;
  ; The engine's ordered choice commits to the first matching alternative and the
  ; enclosing seq does not re-enter it, so each alternative must carry the
  ; disambiguating `=>` itself rather than sharing one after the choice. The
  ; `async`-prefixed form comes first so `async (x) =>` is not misread as a call.
  ;
  ; LEFT-FACTORED (W14-1): the parameter list -- a parenthesized formal_parameters
  ; or a single identifier -- was spelled out twice per prefix (once for each
  ; form); it folds into one `(choice formal_parameters identifier)`, whose two
  ; arms have disjoint first tokens (`(` versus an identifier) so neither re-parses
  ; the other. A bare `async => …` still parses as an arrow whose parameter is
  ; named `async`: the async branch fails to find a `=>` after consuming `async`
  ; as the keyword, and the choice falls through to the non-async branch.
  (rule arrow_function
    (choice
      (seq "async" (field parameters: (choice formal_parameters identifier)) "=>" arrow_body)
      (seq (field parameters: (choice formal_parameters identifier)) "=>" arrow_body)))

  (rule arrow_body (field body: (choice statement_block assignment_expression)))

  ; =============================================================== classes
  (rule class_declaration
    (seq (repeat decorator) "class" (field name: identifier)
         (optional class_heritage) (field body: class_body)))

  (rule class_expression
    (seq (repeat decorator) "class" (optional (field name: identifier))
         (optional class_heritage) (field body: class_body)))

  (rule class_heritage (seq "extends" (field value: expression)))

  (rule class_body
    (seq "{"
         (repeat (choice
                   ";"
                   class_static_block
                   method_definition
                   field_definition))
         "}"))

  (rule class_static_block
    (seq "static" (field body: statement_block)))

  ; A method definition. `get`, `set`, and `async` are also valid method NAMES
  ; (`{ get() {...} }`), and the engine's ordered choice does not re-enter after a
  ; committed optional, so the accessor/async/generator prefixes are tried as full
  ; alternatives that each end in a name, with a bare name as the final fallback.
  (rule method_definition
    (seq (repeat decorator)
         (optional "static")
         method_head
         (field parameters: formal_parameters)
         (field body: statement_block)))

  ; LEFT-FACTORED (W14-1): the two `async` forms (`async *m` and `async m`) shared
  ; their `async` head and re-matched it; they fold into one branch with an
  ; optional `*`. If the `async` branch fails to find a name (a method literally
  ; named `async`, as in `{ async() {} }`), the choice falls through to the bare
  ; name alternative -- the same behaviour as before.
  (rule method_head
    (choice
      (seq "async" (optional "*") (field name: property_key))
      (seq "get" (field name: property_key))
      (seq "set" (field name: property_key))
      (seq "*" (field name: property_key))
      (field name: property_key)))

  (rule field_definition
    (seq (repeat decorator)
         (optional "static")
         (field property: property_key)
         (optional (seq "=" (field value: assignment_expression)))
         semi))

  ; A decorator: `@` then a name, member path, or call (`@x`, `@x.y`, `@x()`).
  ; `member_or_call` already covers all three, so the decorator reuses the
  ; expression chain rather than re-listing the forms.
  (rule decorator
    (seq "@" member_or_call))

  ; =============================================================== expressions
  ; The precedence ladder. `expression_sequence` is the comma operator at the top
  ; (lowest precedence); each rule below binds tighter than the one above.

  (hidden
    (rule expression_sequence
      (seq (field expression: expression)
           (repeat (seq "," (field expression: expression)))))
  )

  (hidden
    (rule expression
      (choice assignment_expression))
  )

  ; Assignment (right-associative). LEFT-FACTORED (explore W2): the operand is
  ; parsed ONCE as a ternary_expression, then an optional assignment tail decides
  ; whether it was the left-hand side of an `=`/compound assignment. The earlier
  ; shape tried `lhs_expression "=" rhs` first and, on a non-assignment, threw that
  ; whole parse away and re-parsed the same operand as a ternary -- the quadratic/
  ; exponential re-work the React corpus exposed. Parsing the operand once removes
  ; that source. arrow_function and yield keep their distinctive prefixes ahead of
  ; it (they fail fast on a non-arrow/non-yield without descending the operand).
  (hidden
    (rule assignment_expression
      (choice
        arrow_function
        yield_expression
        (seq (field left: ternary_expression)
             (optional (seq (field operator: (choice "=" "+=" "-=" "*=" "/=" "%=" "**=" "<<=" ">>=" ">>>="
                                                       "&=" "|=" "^=" "&&=" "||=" "??="))
                            (field right: assignment_expression))))))
  )

  (rule yield_expression
    (seq "yield" (optional "*") (optional assignment_expression)))

  ; A left-hand side for assignment: member/subscript/identifier/pattern.
  (hidden
    (rule lhs_expression
      (choice
        member_or_call
        array_pattern
        object_pattern))
  )

  ; Ternary (right-associative). LEFT-FACTORED (W14-1): the operand is parsed
  ; ONCE as a nullish_expression, then an optional `? … : …` tail decides whether
  ; it was the condition of a conditional. The earlier shape tried the full
  ; conditional first and, on a non-ternary, threw the whole condition parse away
  ; and re-parsed the same operand as a bare nullish_expression -- the same
  ; quadratic/exponential re-descent assignment_expression had, compounded on
  ; every nested level.
  (hidden
    (rule ternary_expression
      (seq (field condition: nullish_expression)
           (optional (seq "?" (field consequence: assignment_expression)
                          ":" (field alternative: assignment_expression)))))
  )

  (hidden
    (rule nullish_expression
      (seq (field left: logical_or_expression)
           (repeat (seq (field operator: "??") (field right: logical_or_expression)))))
  )

  (hidden
    (rule logical_or_expression
      (seq (field left: logical_and_expression)
           (repeat (seq (field operator: "||") (field right: logical_and_expression)))))
  )

  (hidden
    (rule logical_and_expression
      (seq (field left: bitwise_or_expression)
           (repeat (seq (field operator: "&&") (field right: bitwise_or_expression)))))
  )

  (hidden
    (rule bitwise_or_expression
      (seq (field left: bitwise_xor_expression)
           (repeat (seq (field operator: "|") (field right: bitwise_xor_expression)))))
  )

  (hidden
    (rule bitwise_xor_expression
      (seq (field left: bitwise_and_expression)
           (repeat (seq (field operator: "^") (field right: bitwise_and_expression)))))
  )

  (hidden
    (rule bitwise_and_expression
      (seq (field left: equality_expression)
           (repeat (seq (field operator: "&") (field right: equality_expression)))))
  )

  (hidden
    (rule equality_expression
      (seq (field left: relational_expression)
           (repeat (seq (field operator: (choice "===" "!==" "==" "!="))
                        (field right: relational_expression)))))
  )

  (hidden
    (rule relational_expression
      (seq (field left: shift_expression)
           (repeat (seq (field operator: (choice "<=" ">=" "<" ">" "instanceof" "in"))
                        (field right: shift_expression)))))
  )

  (hidden
    (rule shift_expression
      (seq (field left: additive_expression)
           (repeat (seq (field operator: (choice "<<" ">>>" ">>"))
                        (field right: additive_expression)))))
  )

  (hidden
    (rule additive_expression
      (seq (field left: multiplicative_expression)
           (repeat (seq (field operator: (choice "+" "-"))
                        (field right: multiplicative_expression)))))
  )

  ; Multiplicative: `*`, `%`, and division. Division reads its `/` in the `div`
  ; goal so the lexer scans a division operator, not a regex literal.
  (hidden
    (rule multiplicative_expression
      (seq (field left: exponent_expression)
           (repeat (seq (field operator: (choice "*" "%" (goal div slash)))
                        (field right: exponent_expression)))))
  )

  (hidden
    (rule exponent_expression
      (seq (field left: unary_expression)
           (repeat (seq (field operator: "**") (field right: unary_expression)))))
  )

  (hidden
    (rule unary_expression
      (choice
        (seq (field operator: (choice "!" "~" "-" "+" "typeof" "void" "delete" "await"))
             (field argument: unary_expression))
        update_expression))
  )

  (hidden
    (rule update_expression
      (choice
        (seq (field operator: (choice "++" "--")) (field argument: unary_expression))
        (seq (field argument: postfix_expression) (optional (field operator: (choice "++" "--"))))))
  )

  (hidden
    (rule postfix_expression (field expr: member_or_call))
  )

  ; Member / call / subscript chain over a primary. Left-associative tail: each
  ; `.prop`, `[index]`, `(args)`, `?.x`, or template-tag binds tighter than the
  ; binary operators above.
  (hidden
    (rule member_or_call
      (seq (field head: call_head)
           (repeat call_tail)))
  )

  (hidden
    (rule call_head
      (choice
        new_expression
        primary_expression))
  )

  ; A chain link. Optional chaining `?.` may stand before a member (`a?.b`), a
  ; subscript (`a?.[i]`), or a call (`a?.(x)`); a plain `.member`, `[index]`,
  ; `(args)` call, or a tagged-template `fn`...`` are the non-optional links.
  (rule call_tail
    (choice
      (seq "?." (field property: member_property))
      (seq "?." "[" (field index: expression_sequence) "]")
      (seq "?." (field arguments: arguments))
      (seq "." (field property: member_property))
      (seq "[" (field index: expression_sequence) "]")
      (field arguments: arguments)
      (field template: template_string)))

  (hidden
    (rule member_property (choice identifier_name private_property_identifier))
  )

  ; member_expression / subscript_expression / call_expression as standalone
  ; rules so decorators and a few other spots can name them. They reuse the chain.
  (rule member_expression
    (seq (field object: primary_expression)
         (repeat (seq (optional "?.") "." (field property: member_property)))))
  ; (member_property already permits reserved words via identifier_name.)

  (rule call_expression
    (seq (field function: primary_expression)
         (repeat call_tail) (field arguments: arguments)))

  (rule new_expression
    (seq "new" (field constructor: call_head) (optional (field arguments: arguments))))

  (rule arguments
    (seq "(" (optional (seq argument (repeat (seq "," (optional argument))))) ")"))

  (hidden
    (rule argument (choice spread_element assignment_expression))
  )

  (rule spread_element (seq "..." (field value: assignment_expression)))

  (rule parenthesized_expression
    (seq "(" expression_sequence ")"))

  ; Primary expressions. The `expr` goal is active here so a leading `/` opens a
  ; regex literal (the `slash` trigger redirects to `regex`).
  ;
  ; `identifier` is reserved-word-guarded (see its token), so a keyword spelling
  ; is not eaten here as a plain identifier. The keyword-led forms still precede
  ; `identifier` so the more specific production wins where both could start
  ; (e.g. `function` opens a function_expression, not an error).
  (hidden
    (rule primary_expression
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
          jsx_element
          array
          object
          parenthesized_expression
          identifier)))
  )

  ; `import.meta` (a meta-property) and `new.target`. The dotted forms are spelt
  ; out because both halves are reserved words, so neither is a member chain over
  ; a plain identifier.
  (rule meta_property
    (choice (seq "import" "." "meta")
            (seq "new" "." "target")))

  ; Dynamic import: `import(specifier)` is a call expression whose callee is the
  ; `import` keyword. The arguments tail in the member/call chain then applies, so
  ; `import("a").then(...)` chains like any other call.
  (rule import_expression (seq "import" (field arguments: arguments)))

  ; =============================================================== JSX
  ; A JSX element is either a paired element with children, or a self-closing
  ; tag. A fragment `<>...</>` is the paired form with an empty element name.
  ; The self-closing form is tried first so `<Foo />` is not misread as an
  ; opening tag with a stray `/`.
  (rule jsx_element
    (choice jsx_self_closing_element jsx_paired_element))

  (rule jsx_paired_element
    (seq (field open_tag: jsx_opening_element)
         (field children: jsx_children)
         (field close_tag: jsx_closing_element)))

  ; The child list scans under `extras: none` so whitespace between and inside
  ; children stays significant text; a `{ ... }` expression hole re-enables
  ; extras for the JS expression it holds.
  (rule jsx_children extras: none
    (repeat (choice jsx_element jsx_expression html_character_reference jsx_text)))

  ; The tag rules re-enable extras (`extras: all`) because a tag may be reached
  ; from inside the `extras: none` child list (a nested element), yet a tag's
  ; internals -- the spaces between attributes -- are ordinary whitespace.
  (rule jsx_opening_element extras: all
    (seq "<" (optional (seq (field name: jsx_element_name)
                            (repeat (field attribute: jsx_attribute_item))))
         ">"))

  (rule jsx_closing_element extras: all
    (seq "</" (optional (field name: jsx_element_name)) ">"))

  (rule jsx_self_closing_element extras: all
    (seq "<" (field name: jsx_element_name)
         (repeat (field attribute: jsx_attribute_item))
         "/>"))

  ; An element name: a dotted member path (`Foo.Bar.Baz`), a namespaced name
  ; (`Foo:Bar`), or a bare name. The member and namespace forms are tried before
  ; the bare name so the whole path is consumed.
  (rule jsx_element_name
    (choice
      jsx_member_name
      jsx_namespace_name
      jsx_name))

  (rule jsx_name (choice jsx_identifier identifier))

  (rule jsx_member_name
    (seq jsx_name "." jsx_name (repeat (seq "." jsx_name))))

  (rule jsx_namespace_name (seq jsx_name ":" jsx_name))

  ; An attribute is either a `name` / `name=value` pair or a spread expression
  ; (`{...props}`).
  (rule jsx_attribute_item
    (choice jsx_attribute jsx_expression))

  (rule jsx_attribute
    (seq (field name: jsx_attribute_name)
         (optional (seq "=" (field value: jsx_attribute_value)))))

  (rule jsx_attribute_name
    (choice jsx_namespace_name jsx_name))

  (rule jsx_attribute_value
    (choice jsx_string jsx_expression jsx_element))

  ; A JSX attribute string: like a JS string but with raw newlines allowed and
  ; `&entity;` references scanned as their own nodes. `extras: none` keeps the
  ; quote-to-quote content from being split by whitespace handling.
  (rule jsx_string extras: none
    (choice
      (seq "\"" (repeat (choice html_character_reference jsx_double_string_fragment)) "\"")
      (seq "'"  (repeat (choice html_character_reference jsx_single_string_fragment)) "'")))

  ; A JSX expression hole `{ ... }`. It may be empty (`{}`), hold an expression
  ; sequence, or hold a spread. `extras: all` restores ordinary whitespace and
  ; comment handling for the JS expression, since the enclosing child list runs
  ; under `extras: none`.
  (rule jsx_expression extras: all
    (seq "{" (optional (choice spread_element expression_sequence)) "}"))

  ; Arrays allow elision (empty slots), so a comma may stand with no element on
  ; either side: `[ , item2 ]`, `[ a, , b ]`. Each slot is an optional element
  ; and commas separate slots.
  (rule array
    (seq "[" (optional (seq (optional array_element)
                            (repeat (seq "," (optional array_element))))) "]"))

  (rule array_element (choice spread_element assignment_expression))

  (rule object
    (seq "{" (optional (seq object_member (repeat (seq "," (optional object_member))))) "}"))

  (rule object_member
    (choice
      spread_element
      method_definition
      pair
      (field shorthand: identifier)))

  (rule pair
    (seq (field key: property_key) ":" (field value: assignment_expression)))

  (rule property_key
    (choice
      identifier_name
      private_property_identifier
      string
      number
      computed_property_name))

  (rule computed_property_name
    (seq "[" (field expression: assignment_expression) "]"))

  ; Template string: backtick, then runs of template chars / escapes / `${ ... }`
  ; substitutions, then a closing backtick. Substitutions hold a full expression,
  ; and a template literal inside a substitution re-enters this rule, so nesting
  ; works.
  ; Template string: backtick, then a run of substitutions, literal text, and
  ; lone dollars, then a closing backtick. Substitution is tried before the lone
  ; `$` so `${...}` is read as a hole and a bare `$` falls through to `dollar`.
  ; `extras: none` keeps comments and whitespace from being consumed between the
  ; parts of a template literal -- a `//` or a run of spaces inside backticks is
  ; literal content, not trivia. The substitution re-enables extras (`extras:
  ; all`) so the expression in a `${ ... }` hole gets ordinary whitespace and
  ; comment handling back.
  (rule template_string extras: none
    (seq backtick
         (repeat (choice template_substitution template_chars dollar))
         "`"))

  (rule template_substitution extras: all
    (seq "${" expression_sequence "}")))
