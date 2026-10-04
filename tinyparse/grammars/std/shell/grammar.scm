; grammar.scm — std.shell, the standard POSIX shell grammar.
;
; The POSIX shell command language. Unlike most grammars, the newline is
; significant: it terminates a command rather than being insignificant trivia,
; so a list of commands is delimited by newlines. Spaces and tabs separate the
; words of a command and are trivia; a comment runs from "#" to end of line and
; is also trivia.
;
; S2 builds the POSIX word: a concatenation of parts (bare runs, single- and
; double-quoted strings, and backslash escapes) with no blanks between them, so
; that 'a b' is one word and abc'def'"ghi" is one word of three parts.
;
; S3 generalizes the simple command to its full POSIX shape (POSIX §2.9.1): a
; command prefix of variable assignments and redirections, an optional command
; word, and a suffix of arguments and redirections. The semantic rule that an
; assignment NAME=value is an assignment only BEFORE the command word — and an
; ordinary argument after it — falls straight out of the prefix/suffix split:
; the prefix admits Assignment, the suffix admits only Word, so `echo A=1` binds
; A=1 as an argument while `A=1 echo` binds it as an assignment. Redirections
; (POSIX §2.7) are admitted in both prefix and suffix.
;
; S4 adds the command-composition layers above the simple command (POSIX §2.9.2,
; §2.9.3): a pipeline joins commands with "|" (optionally negated by a leading
; "!"); an and-or list joins pipelines with "&&"/"||"; and a complete command is
; a sequence of and-or lists separated or terminated by ";" or "&". The precedence
; falls out of the nesting — a pipeline binds tighter than "&&"/"||", which bind
; tighter than ";"/"&" — and every layer is left-factored (no left recursion).
;
; Compound commands (if/for/while/case), function definitions, expansions, and
; heredocs (<<, <<-) are added in later steps.
;
; This is OUR grammar. It is ported (the LANGUAGE, not the code) from the
; build-time reference reference/tree-sitter-bash; node names follow tree-sitter
; where reasonable, rendered in CamelCase per the tinyparse convention.

(tinyparse version: "0.1")

(module std.shell)

(grammar shell
  start: Program

  ; --- trivia
  ; Inter-word whitespace: spaces and tabs only. The newline is NOT trivia in
  ; shell — it terminates a command — so it is excluded from this class.
  (token Space (regex "[ \t]+"))

  ; A comment runs from "#" to (but not including) the end of the line.
  (token Comment (seq "#" (repeat (not "\n" any))))

  (extras
    (trivia Space)
    (trivia Comment))

  ; --- tokens
  ; The newline terminates a command. It is a real token, not trivia.
  (token Newline "\n")

  ; A bare run of ordinary word bytes: one or more bytes that are neither
  ; whitespace nor a byte the word grammar gives its own structure. Excluded:
  ; space, tab, newline; the metacharacters | & ; ( ) < > ; the quote bytes ' ";
  ; the escape \ ; and the expansion bytes $ ` that S6 will give structure. The
  ; "#" is NOT excluded, so a mid-word "#" (as in a#b) stays part of the word; a
  ; "#" at a word boundary is consumed first by the Comment extra (see below).
  ; The class requires at least one byte (the +).
  (token RawWord (regex "[^ \t\n|&;()<>'\"\\\\$`]+"))

  ; A reserved-word-guarded bare word: the same byte class as RawWord, but the
  ; matched slice is REJECTED when it spells a POSIX reserved word (POSIX §2.4).
  ; A reserved word is special only as a complete, unquoted word in COMMAND
  ; position, and `(reserved …)` (the same not-keyword guard std.javascript uses
  ; for its identifier token) encodes exactly that: the inner pattern matches the
  ; whole bare word greedily, then the match is dropped if the slice equals a
  ; reserved spelling. The greedy match supplies the word boundary for free —
  ; `fi` is reserved but `fish` matches as `fish` (slice ≠ `fi`), and `done2`,
  ; `!a`, `{x` are all fine. Used only for the FIRST part of a command name (see
  ; CommandName); an argument may be a reserved spelling, so it uses RawWord.
  ;
  ; The set is the POSIX reserved words plus `!` (so a bare `!` cannot be a
  ; command name — the pipeline's leading bang handles standalone negation, while
  ; `!a` matches as the word `!a`) and the brace-group delimiters `{`/`}` (which
  ; POSIX requires to be complete words). `(` and `)` are NOT here: they are
  ; operators (punctuation), not delimited reserved words, and are already
  ; excluded from the RawWord byte class.
  (token ReservedWord
    (reserved (regex "[^ \t\n|&;()<>'\"\\\\$`]+")
      "if" "then" "elif" "else" "fi"
      "for" "in" "do" "done"
      "while" "until"
      "case" "esac"
      "{" "}" "!"))

  ; The pipeline-negation bang. POSIX makes `!` a reserved word, special only as a
  ; complete, DELIMITED word: `! cmd` negates, but `!a` is the single command word
  ; `!a`. The trailing zero-width negative lookahead — `!` must NOT be followed by
  ; a RawWord byte — encodes the delimiting: `! ` / `!<newline>` / `!(` match the
  ; bang, while `!a` does not, so `!a` falls through to a RawWord command name.
  (token Bang
    (seq "!" (not (regex "[^ \t\n|&;()<>'\"\\\\$`]"))))

  ; The delimited reserved-keyword tokens. POSIX §2.4/§2.10.2 recognizes a
  ; reserved word only as a COMPLETE, DELIMITED word — `ifx` is the word `ifx`,
  ; not `if` glued to `x`. A bare string literal in a rule matches a PREFIX, so a
  ; raw `"if"` would steal the `if` from `ifx` and split a longer word; the
  ; compound rules below use these tokens instead, so a glued keyword is rejected.
  ;
  ; Each token is the keyword spelling followed by a ZERO-WIDTH right-boundary
  ; assertion: the next byte must NOT be a RawWord byte (the same class verbatim,
  ; copied from the RawWord/Bang tokens). The assertion is written as
  ; `(not GUARD (repeat GUARD))`. The two-argument `(not GUARD BODY)` asserts GUARD
  ; does not match here, then matches BODY; with BODY the repeat of the same word
  ; class, BODY matches ZERO bytes (the guard already proved the next byte is not a
  ; word byte), so the whole assertion consumes nothing. The single-argument form
  ; `(not GUARD)` is NOT used here: it consumes one byte and fails at end of input,
  ; which would both swallow a following separator (`done;`) and reject a terminator
  ; at EOF (`…fi`) — see the Bang token, where consuming the delimiter is harmless
  ; because a command always follows the bang.
  ;
  ; The assertion succeeds at any word delimiter — whitespace, an operator, a
  ; quote/`$`/backtick, or end of input — and fails when the next byte would
  ; continue the word, so `if x`, `if(`, `if;`, `if\n`, and a trailing `fi` at EOF
  ; all match, while `ifx` does not. Terminators (`then`/`fi`/`do`/`done`/…) carry
  ; the boundary too, so `thenx` is not read as `then`. The brace-group delimiters
  ; `{`/`}` are reserved words as well (LBrace/RBrace); `(`/`)`, `;;`, `|` are
  ; operators (punctuation), not delimited words, and stay bare literals.
  (token If    (seq "if"    (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token Then  (seq "then"  (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token Elif  (seq "elif"  (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token Else  (seq "else"  (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token Fi    (seq "fi"    (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token For   (seq "for"   (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token In    (seq "in"    (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token Do    (seq "do"    (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token Done  (seq "done"  (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token While (seq "while" (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token Until (seq "until" (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token Case  (seq "case"  (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token Esac  (seq "esac"  (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token LBrace (seq "{"    (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))
  (token RBrace (seq "}"    (not (regex "[^ \t\n|&;()<>'\"\\\\$`]") (repeat (regex "[^ \t\n|&;()<>'\"\\\\$`]")))))

  ; A single-quoted string: literal bytes up to the next "'". POSIX single quotes
  ; have no escapes, so the body is any byte that is not the closing quote.
  (token SingleQuoted
    (seq "'" (repeat (not "'" any)) "'"))

  ; An escape sequence outside quotes: a backslash followed by one byte, which is
  ; taken literally (so "a\ b" is one word, the escaped space not a separator).
  (token EscapeSequence
    (seq "\\" any))

  ; A portable shell name: a letter or underscore, then letters, digits, and
  ; underscores. This is the left-hand side of an assignment word (POSIX NAME).
  (token Name (regex "[a-zA-Z_][a-zA-Z0-9_]*"))

  ; --- heredoc machinery (S7, POSIX §2.7.4 Here-Document)
  ; A here-document redirection (`<<DELIM` / `<<-DELIM`) names a delimiter on the
  ; command line; the body — the lines that follow the command line's terminating
  ; newline, up to a line that equals DELIM — is matched only AFTER that newline.
  ; The delimiter is unknown until parse time, so it is captured with (defer …)
  ; into a per-command-line FIFO slot and the body drained with (parse-deferred …)
  ; once the newline has been consumed. The mechanism is the engine's deferred-slot
  ; / parametrized-lexical-goal feature (see docs/archive/lex-goal-implementation.md and
  ; examples/heredoc/heredoc.scm); here the body is OPAQUE content lines (a heredoc
  ; body is data, not shell code), which is more POSIX-faithful than the demo.
  ;
  ; HeredocDelimiterWord is the captured delimiter: an UNQUOTED bare run (S7 scope).
  ; The same byte class as RawWord — what the body must later match verbatim, so a
  ; quoted delimiter (<<'EOF') is a refinement deferred to a later step.
  (token HeredocDelimiterWord (regex "[^ \t\n|&;()<>'\"\\\\$`]+"))

  ; The parametrized closing-delimiter line: no static pattern. (parse-deferred …)
  ; instantiates it by substituting `delimiter' with the captured bytes, then runs
  ; HeredocBody against the body. Two entries: `<<` ends at a delimiter line with
  ; no leading blanks; `<<-` strips leading TABS, so its closing line may be
  ; tab-indented. Both share the one parametrized token; the body rule chooses the
  ; goal mode by which redirect deferred it. We use a single tolerant pattern that
  ; allows optional leading tabs, so one (deferred) HeredocBody rule serves both
  ; `<<` and `<<-`: an un-indented delimiter (the `<<` case) matches the
  ; zero-tab arm of (repeat "\t"), and a tab-indented one (the `<<-` case) the rest.
  ;
  ; KNOWN LENIENCY (F2, accept-level). The tolerant `(repeat "\t")` means plain `<<`
  ; ALSO accepts a TAB-indented closing delimiter line, even though POSIX §2.7.4
  ; strips leading tabs only for `<<-`. The strict fix needs per-item goal selection
  ; — the deferred item would carry whether it came from `<<` or `<<-` and pick a
  ; tab-stripping vs exact-match pattern accordingly — which is an engine enhancement
  ; (the parametrized lex-goal currently has one pattern for both). We keep the
  ; tolerant pattern because the strict alternative `(seq delimiter "\n")` (no tab
  ; arm) would wrongly REJECT a valid tab-indented `<<-` closer. So a tab-indented
  ; `<<` closer is accepted where POSIX would treat it as content; this is a
  ; deliberate accept-level over-acceptance, not a body-mangling defect.
  ;
  ; KNOWN DEVIATION (F3). The pattern requires a trailing `"\n"`, so a closing
  ; delimiter on the FINAL line with NO trailing newline (`cat <<EOF\nbody\nEOF` at
  ; end of input) is not recognized as the closer. This is a minor deviation: a
  ; conformant here-document delimiter line ends with a newline.
  (token HeredocDelimLine)

  (lex-goal heredoc-body-mode
    HeredocDelimLine: (param delimiter (seq (repeat "\t") delimiter "\n")))

  ; The optional IO number of a redirection (POSIX IO_NUMBER): a run of digits
  ; that, to count as a file-descriptor number, must abut the redirection
  ; operator with no intervening blank. That adjacency is not enforced by this
  ; token alone — it is the `extras: none` Redirect rule below that forbids a
  ; Space between this number and the operator, so `2>file` redirects fd 2 while
  ; `2 >file` leaves the `2` as an ordinary argument word.
  (token IoNumber (regex "[0-9]+"))

  ; --- expansion tokens (S6, POSIX §2.6 Word Expansions)
  ; A special parameter (POSIX §2.5.2) and the positional parameters (§2.5.1):
  ; the one-character special names @ * # ? $ ! - 0, and a multi-digit positional
  ; parameter $1, $2, … ($N for N>9 still needs braces in real shells, but a bare
  ; run of digits is the more permissive — and harmless — accept-level form here).
  ; `0` is listed in the one-char class; the [1-9][0-9]* alternative is tried first
  ; so $12 reads as positional 12, not $1 glued to a literal 2.
  (token SpecialParam
    (choice (regex "[1-9][0-9]*") "@" "*" "#" "?" "$" "!" "-" "0"))

  ; A literal dollar (POSIX §2.6.2): a `$` that does NOT begin an expansion is an
  ; ordinary character. The zero-width negative lookahead — `$` must not be followed
  ; by a name byte, a special-parameter byte (including a digit, since `$5` is the
  ; positional parameter 5), `{`, or `(` — leaves `echo $`, a trailing `$` at end of
  ; word, and `$.` / `$ ` as literal dollars, while every real expansion start is
  ; claimed by Expansion before this fallback is tried. It is matched as the last
  ; word part, so it never steals a `$` that opens an expansion.
  (token DollarLiteral
    (seq "$" (not (regex "[a-zA-Z_0-9@*#?$!{(-]")
            (repeat (regex "[a-zA-Z_0-9@*#?$!{(-]")))))

  ; The braced-expansion operators (POSIX §2.6.2): parameter-substitution operators
  ; with and without the leading colon (use-default `:-`/`-`, assign-default
  ; `:=`/`=`, indicate-error `:?`/`?`, alternative `:+`/`+`), and the pattern-removal
  ; operators (remove-smallest/largest prefix `#`/`##`, remove-smallest/largest
  ; suffix `%`/`%%`). The two-character forms are listed before the one-character
  ; forms they share a prefix with so the longer operator wins the ordered choice.
  (token BracedOperator
    (choice ":-" ":=" ":?" ":+" "##" "%%" "-" "=" "?" "+" "#" "%"))

  ; The interior of a double-quoted string (S6). Inside double quotes the backslash
  ; is literal EXCEPT before $ ` " \ or a newline (POSIX §2.2.3), so a DQEscape is a
  ; backslash followed by one of those, taken as the escaped byte; any other
  ; backslash is an ordinary character (DQBackslash) — `"\n"` is a literal
  ; backslash then `n`, not a newline. DQText is a run of bytes that are none of
  ; the closing quote, an expansion start ($ or `), or a backslash — the runs of
  ; plain text between the expansions and escapes.
  (token DQEscape (seq "\\" (regex "[$`\"\\\\\n]")))
  ; A lone literal backslash: a backslash NOT forming a DQEscape (tried after it
  ; in the DoubleQuoted body, so `\"`/`\$`/`\\` are still escapes). Kept distinct
  ; from DQText, which excludes backslash, so it cannot greedily swallow the `\`
  ; of a `\"` and close the string early.
  (token DQBackslash "\\")
  (token DQText (regex "[^\"$`\\\\]+"))
  ; A literal dollar INSIDE double quotes (POSIX §2.2.3): a `$` that does not begin
  ; an expansion is an ordinary character here too — `"s$"`, `"s/$/x/"`, `"$ "`. The
  ; lookahead is the same not-an-expansion-start guard as the unquoted DollarLiteral,
  ; so a real `$name`/`${…}`/`$(…)`/`$((…))` is still claimed by Expansion (tried
  ; first in the DoubleQuoted body) and only a stranded `$` falls through to this.
  ; DQText excludes `$`, so without this fallback a literal `$` would have no rule to
  ; match and the string parse would fail at the dollar.
  (token DQDollar
    (seq "$" (not (regex "[a-zA-Z_0-9@*#?$!{(-]")
            (repeat (regex "[a-zA-Z_0-9@*#?$!{(-]")))))

  ; A run of arithmetic content (S6): bytes that are neither a parenthesis nor an
  ; expansion start (`$`/backtick). The ArithmeticExpansion rule below interleaves
  ; these with recursively-balanced parenthesised groups AND with nested expansions,
  ; so the content of $((…)) is balanced text up to the outermost `))` — it need not
  ; be a real arithmetic expression for accept-level. Excluding `$`/backtick is what
  ; lets `$(($a + $b))` see BOTH operand expansions rather than swallowing the
  ; second `$b` into a plain run.
  (token ArithChar (regex "[^()$`]+"))

  ; The interior of a backtick command substitution (S6, POSIX §2.6.3). Opaque:
  ; a backslash escape (`\`` keeps a literal backtick out of the closing delimiter)
  ; or any byte that is not the closing backtick.
  (token BacktickBody (repeat (choice (seq "\\" any) (not "`" any))))

  ; One opaque line of here-document body: any bytes up to and including the
  ; newline. A heredoc body is DATA — it is not parsed as shell — so a content
  ; line is the whole raw line, leading tabs (for `<<-`) and all. extras: none on
  ; HeredocBody keeps the newline significant so each content line is one line.
  (token HeredocContentLine (seq (repeat (not "\n" any)) "\n"))

  ; --- rules
  ; A program is a sequence of newline-separated complete commands, tolerating
  ; blank lines anywhere (leading, trailing, between commands) and a final
  ; command with no terminating newline. Each complete command before the last is
  ; followed by one or more newlines; the optional trailing CompleteCommand picks
  ; up an unterminated final line such as `echo hi` with no closing "\n".
  (rule Program
    (seq (repeat Newline)
         (repeat (seq CommandLine (repeat Newline)))
         (optional CompleteCommand)
         (repeat Newline)
         EOF))

  ; A command line (S7): a complete command, its terminating newline, then any
  ; here-document bodies the line opened, drained in FIFO order. The here-doc body
  ; follows the command line's terminating newline (POSIX §2.7.4), so the slot is
  ; declared here and drained AFTER the mandatory Newline — the drain is the last
  ; element of this committed seq, the safe position the spec requires. With no `<<`
  ; on the line the slot is empty and the drain is a no-op, so a plain command line
  ; keeps its prior shape. The trailing unterminated final command in Program
  ; (`echo hi` with no newline) is handled by Program's optional CompleteCommand and
  ; carries no heredoc — a heredoc needs the terminating newline this rule supplies.
  ;
  ; The terminating newline AND the drain are folded into HeredocTerminator, an
  ; `extras: none` wrapper, NOT left as a bare `Newline` + `(parse-deferred …)` — see
  ; HeredocTerminator for why. A here-document body must begin EXACTLY at the byte
  ; after the command line's terminating newline (POSIX §2.7.4), with NO
  ; leading-trivia skip; draining under CommandLine's normal extras would let the
  ; HeredocBody CALL eat the body's first-line leading whitespace (and a leading `#`
  ; as a Comment) before the `extras: none` body started.
  (hidden (rule CommandLine
    (seq (deferred-slot heredoc-bodies)
         CompleteCommand
         HeredocTerminator)))

  ; A complete command (POSIX §2.9.3): `list = and_or (sep and_or)*` then an
  ; optional trailing separator (`complete_command = list separator?`). Every
  ; non-trailing separator MUST be followed by another and-or list, so an empty
  ; command between two separators (`a;;b`) and a dangling and-or (`a &&`, whose
  ; bare `&` would otherwise be mis-read as a background separator) are rejected
  ; rather than silently accepted. Only ONE trailing `;`/`&` is allowed. The ";"
  ; sequences the commands; the "&" marks the preceding command to run in the
  ; background (kept as a plain separator token for now, not yet a node).
  (rule CompleteCommand
    (seq AndOrList
         (repeat (seq (field sep: (choice ";" "&")) AndOrList))
         (optional (field sep: (choice ";" "&")))))

  ; An and-or list (POSIX §2.9.3): pipelines joined left-to-right by "&&" (run the
  ; next only on success) or "||" (run the next only on failure). A linebreak is
  ; permitted after the operator, so `a &&\n b` is one list. Left-factored as
  ; `Pipeline (op Pipeline)*` — no left recursion.
  (rule AndOrList
    (seq Pipeline
         (repeat (seq (field op: (choice "&&" "||")) (optional Newline) Pipeline))))

  ; A pipeline (POSIX §2.9.2): one or more commands joined by "|", the standard
  ; output of each feeding the standard input of the next, optionally negated by a
  ; leading "!". A linebreak is permitted after "|", so `a |\n b` is one pipeline.
  ; Left-factored as `command (| command)*` — no left recursion.
  ;
  ; `!` is a reserved word, special only as a delimited word: `! cmd` negates the
  ; pipeline, while `!a` (no blank) is the single command word `!a`. The leading
  ; bang here matches a standalone `!`; the `!a` case never reaches it because the
  ; command name's reserved guard rejects only the bare slice `!` (not `!a`), so
  ; `!a` is taken as a command word by SimpleCommand (see ReservedWord/CommandName).
  (rule Pipeline
    (seq (optional (field bang: Bang))
         (field command: Command)
         (repeat (seq "|" (optional Newline) (field command: Command)))))

  ; A command is the unit a pipeline composes (POSIX §2.9.4): a compound command,
  ; a function definition, or a simple command. The ordered choice resolves the
  ; three by leading shape:
  ;   CompoundCommand FIRST — a reserved-word- or punctuation-led form (`if`,
  ;     `for`, `(`, `{`, …) dispatches to its compound rule before the simple
  ;     branch can attempt — and fail — to read a bare reserved word as a command
  ;     name.
  ;   FunctionDefinition NEXT — `fname ( )` (POSIX §2.9.5). It must precede
  ;     SimpleCommand so `foo()` is one function definition rather than the command
  ;     `foo` followed by a stray `(`. It cannot precede CompoundCommand: a
  ;     subshell `( … )` also opens with `(`, but a function name is a Name (never
  ;     `(`), so the two never collide and CompoundCommand-first is safe.
  ; SimpleCommand LAST — a bare `foo` with no `()` falls through to here, because
  ;     FunctionDefinition needs the `( )` after the name and backtracks without it.
  ; The rule is hidden: when it matches a single child (always, by construction) it
  ; collapses, leaving `Pipeline -> IfClause` / `-> FunctionDefinition` / `->
  ; SimpleCommand`.
  (hidden (rule Command
    (choice CompoundCommand FunctionDefinition SimpleCommand)))

  ; A compound command (POSIX §2.9.4): a grouping or control-flow construct,
  ; followed by an optional redirect list. Each alternative is reserved-word- or
  ; punctuation-led, so the ordered choice is unambiguous (no two share a leading
  ; word).
  ;
  ; POSIX §2.9.4 attaches a `redirect_list` to a compound command, so the whole
  ; construct's input and output may be redirected as a unit — `if …; fi >out`,
  ; `( … ) >out`, `{ …; } 2>err`, `while …; done >>log`. The trailing
  ; `(repeat redirect: Redirect)` captures that; with zero redirects (the common
  ; case) it is empty and the tree shape of a redirect-less compound is unchanged.
  ; A function body is a CompoundCommand too, so a function's body-redirects
  ; (`f() { cat; } >out`) come for free through this same suffix.
  (rule CompoundCommand
    (seq
      (choice IfClause ForClause WhileClause UntilClause CaseClause Subshell BraceGroup)
      (repeat (choice (field redirect: HeredocRedirect) (field redirect: Redirect)))))

  ; A compound-list (POSIX §2.9.4 `compound_list`): the body shared by every
  ; compound command — a sequence of and-or lists separated by ";", "&", or
  ; newlines, with optional leading and trailing linebreaks. Unlike CompleteCommand
  ; (which a Program separates by newlines), a compound-list folds the newline INTO
  ; the separator set, because a compound body may run across lines freely
  ; (`if c\nthen\n a\n b\nfi`). It terminates naturally at a reserved terminator
  ; word (`then`/`do`/`done`/`fi`/`else`/`elif`/`esac`/`}`): the and-or list's
  ; command name reserved-guard rejects that word, so the repeat stops and the
  ; enclosing compound command goes on to match its own terminator keyword. The
  ; trailing separators are optional so `a; b` and `a; b;` both close cleanly.
  ;
  ; S7 restructures the body into per-PHYSICAL-LINE units (CompoundListLine) so a
  ; here-document inside a compound body — `if true; then cat <<EOF\nx\nEOF\n fi` —
  ; drains its body after that line's newline. Each line declares its own
  ; `heredoc-bodies` slot (a fresh instance per invocation) and, when terminated by
  ; a Newline, drains it; a per-line slot is required because parse-deferred drains
  ; the whole slot from index 0 each time, so one slot cannot serve many lines. The
  ; separator structure is preserved — `;`/`&` join and-or lists within a line, a
  ; Newline ends a line — so SimpleCommand and `sep:` shapes are unchanged for the
  ; heredoc-free body. The terminator-keyword stop is unchanged: the line's first
  ; AndOrList fails to read the reserved word as a command name and the line repeat
  ; ends, leaving the keyword for the enclosing compound command.
  ; The body is leading blank-line separators, then zero or more NEWLINE-terminated
  ; lines (each declares its own slot and drains its here-documents after the
  ; newline), an optional final UNTERMINATED line (a `;`/`&`-closed or
  ; terminator-followed line with no newline — and so no here-document), and any
  ; trailing separators. Splitting the newline-terminated case (CompoundListLine,
  ; which carries the heredoc drain) from the unterminated case (CompoundListTail)
  ; lets the drain consume its own newline as the last element of a committed seq
  ; (the safe deferred-slot position) while still admitting a final line that ends
  ; at a reserved terminator with no newline (`if c; then a; fi`). The trailing
  ; `(repeat (choice ";" "&" Newline))` absorbs leftover bare separators exactly as
  ; the prior CompoundList did — including BOTH `;` of a case item's `;;`, so
  ; CaseItem's own `;;` stays optional.
  (rule CompoundList
    (seq (repeat (choice ";" "&" Newline))
         (repeat CompoundListLine)
         (repeat (choice ";" "&" Newline))))

  ; A line of a compound-list: a chain of and-or lists joined by ";"/"&" (each
  ; interior separator FOLLOWED by another and-or list, so `;;` is not absorbed
  ; mid-chain), an optional single trailing ";"/"&", then an OPTIONAL terminating
  ; Newline and here-document drain. Hidden so it collapses into the enclosing
  ; CompoundList.
  ;
  ; LEFT-FACTORED (S8). This rule once had a twin, CompoundListTail, for the final
  ; line that ends NOT at a newline but at a reserved word / `)` / `}` / `;;` / EOF:
  ; CompoundList tried `(repeat CompoundListLine) (optional CompoundListTail)`. The
  ; two shared their whole and-or-chain prefix and differed only in the terminator
  ; (CompoundListLine demanded a Newline; CompoundListTail had none). On a body whose
  ; final line lacked a newline — every `$( … )` / `( … )` whose last command is not
  ; newline-terminated — CompoundListLine parsed the entire and-or chain, FAILED for
  ; lack of the Newline, and CompoundListTail then re-parsed the identical chain.
  ; Because that chain may itself contain a nested command substitution holding the
  ; same construct, the discarded re-parse doubled at every nesting level: with the
  ; rule memo OFF a depth-18 nest of `$(echo …)` took ~1.4s and depth-20 ~5.5s
  ; (roughly 3x per two levels). Folding the terminator into one `(optional …)` parses
  ; the chain ONCE: a present Newline is taken greedily (so a newline-terminated line
  ; behaves exactly as before, draining its here-documents), and the final
  ; newline-less line simply skips the optional. Same accepts/rejects, no re-parse.
  ;
  ; The terminating `sep: Newline` and the drain stay folded into
  ; CompoundHeredocTerminator (the `extras: none` wrapper) rather than a bare
  ; `(field sep: Newline)` + `(parse-deferred …)`, for the same POSIX §2.7.4 reason
  ; CommandLine uses HeredocTerminator: a here-document body must start at the byte
  ; after this line's newline with no leading-trivia skip. See HeredocTerminator. The
  ; slot is declared here (a fresh instance per line); when the optional terminator is
  ; skipped there is no drain, which is correct — a here-document needs the
  ; terminating newline to have a body to drain.
  (hidden (rule CompoundListLine
    (seq (deferred-slot heredoc-bodies)
         AndOrList
         (repeat (seq (field sep: (choice ";" "&")) AndOrList))
         (optional (field sep: (choice ";" "&")))
         (optional CompoundHeredocTerminator))))

  ; An if clause (POSIX §2.9.4): `if list then list (elif list then list)* (else
  ; list)? fi`. The condition and each body are compound-lists, terminated by the
  ; next reserved keyword. Left-factored: the elif clauses are a repeat, the else
  ; clause optional.
  (rule IfClause
    (seq If (field condition: CompoundList)
         Then (field body: CompoundList)
         (repeat (field elif: ElifClause))
         (optional (field else: ElseClause))
         Fi))

  (rule ElifClause
    (seq Elif (field condition: CompoundList)
         Then (field body: CompoundList)))

  (rule ElseClause
    (seq Else (field body: CompoundList)))

  ; A for clause (POSIX §2.9.4): `for name [in word…] sequential-sep do list done`.
  ; The word list and its separator are optional together — `for name; do … done`
  ; iterates over the positional parameters. When present, `in` is followed by zero
  ; or more words and then a ";" or newline before `do`. A linebreak is permitted
  ; after the name and after `in`.
  (rule ForClause
    (seq For (field variable: Name) (repeat Newline)
         (optional (seq In (repeat (field value: Word))))
         (optional (choice ";" Newline)) (repeat Newline)
         Do (field body: CompoundList)
         Done))

  ; While / until clauses (POSIX §2.9.4): `while list do list done` runs the body
  ; while the condition succeeds; `until` runs it while the condition fails. Both
  ; share the `keyword list do list done` shape.
  (rule WhileClause
    (seq While (field condition: CompoundList)
         Do (field body: CompoundList)
         Done))

  (rule UntilClause
    (seq Until (field condition: CompoundList)
         Do (field body: CompoundList)
         Done))

  ; A case clause (POSIX §2.9.4): `case word in linebreak case-item* esac`. Each
  ; item matches one or more "|"-separated glob patterns (with an optional leading
  ; "("), a ")", a compound-list body, and the "`;;`" terminator. POSIX lets the
  ; LAST item before `esac` omit the "`;;`", so it is optional here; the body's
  ; reserved guard stops it at `esac` (or at the next pattern's `)`), and the
  ; absent `;;` on a non-final item is caught because the following pattern would
  ; have to re-open with `)` it cannot reach. The body itself may be empty
  ; (`a) ;;`), so CompoundList is optional.
  (rule CaseClause
    (seq Case (field value: Word) In (repeat Newline)
         (repeat (field item: CaseItem))
         Esac))

  (rule CaseItem
    (seq (optional "(")
         (field pattern: Word)
         (repeat (seq "|" (field pattern: Word)))
         ")"
         (optional CompoundList)
         (optional (seq ";;" (repeat Newline)))))

  ; A subshell (POSIX §2.9.4): `( compound-list )` runs its body in a subshell.
  ; The parentheses are operators (punctuation), so no delimiting word is needed —
  ; `(a)` is a valid subshell.
  (rule Subshell
    (seq "(" (field body: CompoundList) ")"))

  ; A brace group (POSIX §2.9.4): `{ compound-list ; }` groups commands in the
  ; current shell. POSIX requires `{` and `}` to be reserved WORDS (delimited by
  ; blanks/operators), and requires the list to end with a ";" or newline before
  ; the closing `}`. CompoundList already admits — and its trailing-separator
  ; repeat consumes — that ";"/newline, and the `}` is matched as a literal word;
  ; the body's reserved guard rejects a bare `}` as a command name, so the list
  ; stops there. `{` and `}` glued to a word (`{x`, `x}`) are ordinary words, not
  ; the group delimiters, because the reserved guard only fires on the bare slice.
  (rule BraceGroup
    (seq LBrace (field body: CompoundList) RBrace))

  ; A function definition (POSIX §2.9.5): `fname ( ) compound-command`. It binds a
  ; name to a compound command run as the function body.
  ;
  ; The name is a ReservedWord — the same reserved-word-guarded bare run that opens
  ; a CommandName — so a reserved spelling cannot be a function name: `if() {…}`
  ; fails the guard (the `if` slice is reserved) and does not define a function
  ; named `if`. (POSIX requires fname to be a NAME and not a reserved word; the
  ; reserved guard enforces the reserved-word half, and the bare-run byte class is
  ; the same accept-level approximation CommandName uses for the NAME half.)
  ;
  ; The `()` is two operators, so the name's bare run stops before `(` whether or
  ; not a blank intervenes — `foo()` and `foo ()` both parse, the inter-token Space
  ; being trivia. POSIX's `linebreak` between `)` and the body is the optional
  ; Newline repeat, so `foo()\n{ … }` works. The body is any CompoundCommand
  ; (commonly a BraceGroup or Subshell); because CompoundCommand now carries its
  ; own redirect suffix, a redirected body (`f() { cat; } >out`) needs nothing
  ; extra here.
  ;
  ; FunctionDefinition sits between CompoundCommand and SimpleCommand in the Command
  ; choice: after CompoundCommand (so `(`/`{`/`if`/… still dispatch to their
  ; compound rules) and before SimpleCommand (so `foo()` is a function, not the
  ; command `foo` plus a stray `(`); a bare `foo` with no `()` backtracks out of
  ; this rule and is read as a SimpleCommand.
  (rule FunctionDefinition
    (seq (field name: ReservedWord)
         "(" ")"
         (repeat Newline)
         (field body: CompoundCommand)))

  ; --- expansions (S6, POSIX §2.6 Word Expansions)
  ; An expansion is any of the `$`/backtick-introduced forms. ORDER MATTERS: the
  ; longer-prefix forms come first so the shorter ones cannot steal their opener.
  ; `$((` (arithmetic) is tried before `$(` (command substitution), and `${`
  ; (braced parameter expansion) before a bare `$name`/`$1` (simple expansion). The
  ; backtick form is the old-style command substitution. The rule is hidden so it
  ; collapses to whichever specific expansion node matched.
  (hidden (rule Expansion
    (choice ArithmeticExpansion CommandSubstitution Backtick BracedExpansion SimpleExpansion)))

  ; Simple parameter expansion (POSIX §2.6.2): `$name` or `$special`. A `$` not
  ; followed by a valid parameter is handled by the DollarLiteral fallback in Word,
  ; not here. extras: none keeps the `$` glued to its parameter.
  (rule SimpleExpansion extras: none
    (seq "$" (choice (field name: Name) (field name: SpecialParam))))

  ; Braced parameter expansion (POSIX §2.6.2): `${…}`. Three shapes, ordered so a
  ; shared prefix does not mislead the choice:
  ;   length    `${#name}`  — the parameter's length (the leading `#` distinguishes
  ;                            it; tried first so `#` is not read as an operator).
  ;   operator  `${name OP word}` — use/assign-default, error, alternative, and
  ;                            prefix/suffix pattern removal. The operand `word` is
  ;                            a brace-body (Word-like, may nest expansions) up to
  ;                            the matching `}`.
  ;   plain     `${name}`   — the parameter's value.
  ; The parameter is a Name or a special parameter. extras: none keeps the whole
  ; construct contiguous (no blanks around the braces).
  (rule BracedExpansion extras: none
    (seq "${"
         (choice
           (seq "#" (field name: (choice Name SpecialParam)))
           (seq (field name: (choice Name SpecialParam))
                (field op: BracedOperator)
                (optional (field word: BraceBody)))
           (field name: (choice Name SpecialParam)))
         "}"))

  ; The operand of a braced operator (POSIX §2.6.2): shell word text up to the
  ; matching `}`. It may itself contain expansions (`${x:-$y}`), single- and
  ; double-quoted strings, and escapes; BraceText is a run of bytes that are none
  ; of `}`, an expansion start, a quote, or a backslash. extras: none — the body is
  ; contiguous word text, blanks included (`${x:- default}` keeps the leading space
  ; as part of the default).
  (rule BraceBody extras: none
    (repeat1 (choice Expansion SingleQuoted DoubleQuoted EscapeSequence BraceText)))

  (token BraceText (regex "[^}$`'\"\\\\]+"))

  ; Command substitution (POSIX §2.6.3): `$( compound-list )`. The body is the full
  ; command grammar, so `$(echo hi)`, `$(a | b && c)`, and nesting `$(echo $(date))`
  ; all parse by the same recursion that reaches Word→Expansion→CommandSubstitution
  ; again. The `$(` opener is consumed before the recursion, so the cycle is not
  ; left-recursive. Leading and trailing newlines inside the parentheses are folded
  ; into CompoundList's own linebreak handling. The space in `$( (subshell) )` is
  ; what keeps it OUT of arithmetic expansion: `$((` (no space) is arithmetic and is
  ; tried first; `$( (` (a space after the first paren) fails the arithmetic opener
  ; and falls here, where CompoundList matches the inner subshell.
  ;
  ; CommandSubstitution is reached from Word, which is `extras: none`; without
  ; restoring trivia the blanks BETWEEN the words of the inner command (`$(a b)`)
  ; would not be consumed and the body would fail to parse. `extras: all` re-enables
  ; Space/Comment for the recursive command grammar inside the parentheses.
  (rule CommandSubstitution extras: all
    (seq "$(" (field body: CompoundList) ")"))

  ; Arithmetic expansion (POSIX §2.6.4): `$(( expression ))`. For accept-level
  ; conformance the content is treated as BALANCED TEXT up to the OUTERMOST `))`:
  ; ArithGroup matches a parenthesised run recursively, so inner parens like
  ; `$(( (a+b) * c ))` are balanced and do not end the expansion early, while a flat
  ; `$((x))` and `$(($a + $b))` (with nested expansions in the content) also work.
  ; ArithChar is any non-paren run; ArithGroup is a `(`-balanced-`)` group. The two
  ; `)` that finally close the expansion are the literal `))` after the body.
  (rule ArithmeticExpansion extras: none
    (seq "$((" (repeat ArithBody) "))"))

  (hidden (rule ArithBody
    (choice Expansion ArithGroup ArithChar)))

  (rule ArithGroup extras: none
    (seq "(" (repeat ArithBody) ")"))

  ; Backtick command substitution (POSIX §2.6.3), the old form `` `command` ``. The
  ; body is opaque for accept-level: a run of escaped bytes (so `` \` `` keeps a
  ; literal backtick) and any non-backtick byte, up to the closing backtick. Real
  ; nesting in backticks needs backslash escaping, which the BacktickBody escape
  ; clause admits.
  (rule Backtick extras: none
    (seq "`" BacktickBody "`"))

  ; A word is one or more parts concatenated with NO separating trivia, so the
  ; blanks that separate words cannot appear between parts. `extras: none` turns
  ; off the Space/Comment consumption between the parts, which is exactly what
  ; keeps abc'def'"ghi" a single word of three parts. Expansion is FIRST so a `$`
  ; or backtick opens an expansion rather than being stranded; DollarLiteral is the
  ; LAST fallback for a `$` that begins no expansion (`echo $`). RawWord already
  ; excludes `$` and backtick, so `abc$x` splits into RawWord `abc` + Expansion `$x`.
  (rule Word extras: none
    (repeat1 (choice Expansion RawWord SingleQuoted DoubleQuoted EscapeSequence DollarLiteral)))

  ; A double-quoted string (S6, POSIX §2.2.3): now a RULE, not a token, so the
  ; expansions inside it gain structure. Inside double quotes a `$` or backtick
  ; still opens an expansion (Expansion first), the backslash is literal except
  ; before $ ` " \ or newline (DQEscape), and everything else is opaque DQText.
  ; Single quotes (SingleQuoted) stay literal — no expansion fires inside them.
  ; extras: none — the quoted bytes are never touched by trivia.
  (rule DoubleQuoted extras: none
    (seq "\"" (repeat (choice Expansion DQEscape DQBackslash DQDollar DQText)) "\""))

  ; A command name: a Word whose FIRST part, when a bare unquoted run, is checked
  ; against the reserved words (ReservedWord) so a bare reserved spelling cannot be
  ; a command name. This is what makes a compound-list terminate: in command
  ; position, `then`/`fi`/`do`/`done`/`else`/`elif`/`esac`/`}`/`!`/`{` fail to
  ; parse as a command name, the enclosing list's repeat stops, and the compound
  ; command matches its terminator keyword. A quoted or concatenated first part
  ; (`"if"`, `'fi'foo`) bypasses the guard — those are never reserved — because the
  ; quote choices match where ReservedWord (a bare run) cannot. Arguments use the
  ; plain Word, so `echo if done` keeps `if`/`done` as arguments. Like Word, it is
  ; `extras: none` so its parts stay contiguous.
  (rule CommandName extras: none
    (seq (choice Expansion ReservedWord SingleQuoted DoubleQuoted EscapeSequence DollarLiteral)
         (repeat (choice Expansion RawWord SingleQuoted DoubleQuoted EscapeSequence DollarLiteral))))

  ; An assignment word: a NAME, an "=", and an optional value Word. The value is
  ; a full S2 Word and may be absent (VAR= sets the variable to the empty
  ; string). Because a bare RawWord would otherwise swallow `VAR=value` whole,
  ; Assignment is tried before the command word in the prefix; PEG backtracking
  ; makes the fall-through clean — for `echo`, Name matches `echo`, the "=" then
  ; fails, Assignment backtracks, and the command-word branch takes over.
  ;
  ; extras: none keeps the NAME=VALUE token contiguous: no blank may sit around
  ; the `=`, and the optional value cannot jump a blank to grab the next word. So
  ; `VAR= echo` is an empty-value assignment followed by the command `echo`
  ; (POSIX §2.9.1), not `VAR` assigned the value `echo`.
  (rule Assignment extras: none
    (seq (field name: Name) "=" (optional (field value: Word))))

  ; The redirection operators (POSIX §2.7). Multi-character operators are listed
  ; before any single-character operator they share a prefix with, so the longer
  ; operator wins the ordered choice: >> / >& / >| before >, and <& / <> before
  ; <. The heredoc operators << and <<- are their own redirection form
  ; (HeredocRedirect, below), because they capture a delimiter and defer a body
  ; rather than name a target Word; they are NOT in this list.
  (rule RedirectOp
    (choice ">>" ">&" ">|" "<&" "<>" ">" "<"))

  ; The head of a redirection — its optional IO number and operator — scanned
  ; with `extras: none` so no blank may fall between them. This is what enforces
  ; the POSIX IO_NUMBER adjacency: the digits must abut the operator to be read
  ; as a file descriptor. The operator may still be followed by a blank before
  ; the target, because the target is matched outside this rule (back under the
  ; normal extras) in Redirect.
  (hidden (rule RedirectHead extras: none
    (seq (optional (field fd: IoNumber)) RedirectOp)))

  ; A redirection: an optional file descriptor adjacent to a redirection
  ; operator, then a target Word. `cat <in`, `echo x >>log`, `ls 2>err`, and
  ; `cmd 2>&1` are all redirections; the `2` in `2>err`/`2>&1` is the fd.
  (rule Redirect
    (seq RedirectHead (field target: Word)))

  ; A here-document redirection (POSIX §2.7.4): an optional IO number adjacent to
  ; `<<` or `<<-`, then the delimiter word. `<<-` is listed before `<<` so the
  ; longer operator wins the ordered choice. The fd and operator are scanned with
  ; `extras: none` (the same adjacency rule as RedirectHead) so no blank falls
  ; between the fd and the operator; the delimiter then follows under normal extras
  ; (a blank may sit between `<<` and the delimiter — `<< EOF` is valid POSIX).
  ;
  ; The delimiter is captured into the per-command-line `heredoc-bodies` slot with
  ; (defer …): the match builds the same `delimiter:` child it would without the
  ; defer, and additionally enqueues the matched bytes so the slot drain after the
  ; command line's newline can instantiate the closing-delimiter pattern. Multiple
  ; heredoc redirects on one line (`cat <<A <<B`) enqueue A then B; the FIFO drain
  ; matches body A then body B in that order (POSIX §2.7.4).
  (hidden (rule HeredocHead extras: none
    (seq (optional (field fd: IoNumber)) (field op: (choice "<<-" "<<")))))

  (rule HeredocRedirect
    (seq HeredocHead
         (field delimiter: (defer heredoc-bodies HeredocDelimiterWord))))

  ; The here-document terminators (S7, POSIX §2.7.4): the thin `extras: none`
  ; wrappers that CommandLine and CompoundListLine use IN PLACE of a bare terminating
  ; `Newline` followed by `(parse-deferred …)`. There are two — one with an unfielded
  ; Newline (HeredocTerminator, for CommandLine) and one with `sep: Newline`
  ; (CompoundHeredocTerminator, for CompoundListLine) — so each line rule keeps the
  ; exact Newline tree-shape it had before S7's fix.
  ;
  ; WHY a wrapper, and why it must include the Newline. A here-document body begins
  ; EXACTLY at the byte after the command line's terminating newline — no leading
  ; whitespace is stripped and a leading `#` is literal data, not a comment (a heredoc
  ; body is data, never shell). The line rules are NOT `extras: none`: their extras
  ; are Space and Comment. The trap is that the leading-extras skip happens at a CALL
  ; BOUNDARY (the call prologue consumes leading trivia into the caller's sink before
  ; the callee's body — and its `extras: none` — runs). So draining straight from the
  ; line rule, OR through a wrapper that holds only the drain, still lets the CALL into
  ; that wrapper eat the body's first-line leading trivia at offset-after-newline,
  ; under the line rule's extras-enabled scope: a body first line ` EOF` would lose
  ; its space and CLOSE EARLY, and a first line `#foo` would vanish as a Comment.
  ;
  ; Folding the Newline INTO the `extras: none` wrapper fixes it. The CALL into the
  ; wrapper now lands at the command line's TRAILING position (before the newline),
  ; where consuming trivia is correct — it absorbs a trailing `  ` or ` # comment`
  ; that may sit before the newline. Inside the wrapper no_extras is in force, so the
  ; Newline matches the bare `\n` and then `parse-deferred`'s CALL into HeredocBody
  ; runs under no_extras: the body's first line is taken verbatim from column 0.
  ;
  ; The slot still resolves: `parse-deferred` finds the nearest enclosing frame that
  ; declared `heredoc-bodies` by scanning the frame stack OUTWARD by name. The wrapper
  ; declares no slot, so the scan walks PAST its frame to CommandLine /
  ; CompoundListLine (which still hold `(deferred-slot heredoc-bodies)`) and drains
  ; their slot — an extra frame in the path is harmless.
  ;
  ; Both wrappers are `hidden`. A hidden rule collapses (splices its child up in
  ; place) only when it has EXACTLY ONE child. On a heredoc-FREE line the slot is
  ; empty, the drain is a no-op, and the wrapper's sole child is the Newline: it
  ; collapses, so a plain line's tree is byte-identical to the pre-S7 `Newline`
  ; element. On a line that DID open a here-document the wrapper holds two children —
  ; the Newline and the drained HeredocBody — so it does not collapse and a
  ; HeredocTerminator / CompoundHeredocTerminator node stays in the tree around them.
  ; That extra wrapper is the price of folding the Newline into the no_extras scope
  ; (which is what keeps the body's first line intact); the HeredocBody and its
  ; content lines are nested one level deeper but otherwise unchanged.
  (hidden (rule HeredocTerminator extras: none
    (seq Newline
         (parse-deferred heredoc-bodies HeredocBody))))

  (hidden (rule CompoundHeredocTerminator extras: none
    (seq (field sep: Newline)
         (parse-deferred heredoc-bodies HeredocBody))))

  ; The here-document body (POSIX §2.7.4): the lines after the command line's
  ; terminating newline, up to the closing delimiter line. extras: none — the
  ; newline is a significant content byte, not trivia. (parse-deferred …) has
  ; already pushed the instantiated `(seq (repeat "\t") delimiter "\n")` pattern
  ; onto heredoc-body-mode's stack before this rule runs, and pops it on return.
  ; Content is opaque: zero or more raw lines, each guarded by a negative lookahead
  ; that stops the repeat when the closing delimiter line is next; end: is that
  ; delimiter line, matched via the parametrized HeredocDelimLine token.
  (rule HeredocBody extras: none
    (goal heredoc-body-mode
      (seq
        (repeat (not HeredocDelimLine HeredocContentLine))
        (field end: HeredocDelimLine))))

  ; A simple command (POSIX §2.9.1):
  ;   simple_command : cmd_prefix [ cmd_word cmd_suffix ]
  ;                  | cmd_name cmd_suffix
  ; where the prefix is a run of assignments and redirections, and the suffix is
  ; a run of arguments and redirections. Assignment appears only in the prefix,
  ; so a NAME=value after the command word is an ordinary argument Word. The
  ; command word itself may be absent: a prefix alone (`VAR=x`, or `>file`) is a
  ; valid simple command. The prefix branch is tried first; if no assignment or
  ; redirection opens the command, the no-prefix branch matches a plain command
  ; word and its suffix (so `echo hi` parses with no spurious assignment).
  ; The command word is a CommandName (reserved-word-guarded) so a bare keyword
  ; cannot open a simple command; arguments remain plain Words (a keyword spelling
  ; is a fine argument). After an assignment/redirection prefix the command word is
  ; still a CommandName: `A=1 if` would be `A=1` then a reserved word, which the
  ; guard rejects (the compound-command branch handles real `if`).
  ; A heredoc redirection is admitted wherever an ordinary redirection is — in the
  ; prefix and the suffix of a simple command, and in a compound command's redirect
  ; list — so `cat <<EOF`, `cat <<EOF >out`, and `cat <<EOF | grep x` all parse.
  ; HeredocRedirect is tried before Redirect because `<<` shares its `<` prefix
  ; with the `<` and `<&`/`<>` operators; the longer-match ordering keeps `<<` from
  ; being read as a bare `<` redirect.
  (rule SimpleCommand
    (choice
      (seq
        (repeat1 (choice (field assignment: Assignment)
                         (field redirect: HeredocRedirect)
                         (field redirect: Redirect)))
        (optional
          (seq (field name: CommandName)
               (repeat (choice (field redirect: HeredocRedirect)
                               (field redirect: Redirect)
                               (field arg: Word))))))
      (seq (field name: CommandName)
           (repeat (choice (field redirect: HeredocRedirect)
                           (field redirect: Redirect)
                           (field arg: Word)))))))
