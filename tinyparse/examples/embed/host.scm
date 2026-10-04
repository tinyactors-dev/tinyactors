; host.scm -- a Markdown-ish host grammar that DELIMITS fenced code blocks but
; does NOT parse what is inside them.
;
; A document is a run of fenced code blocks,
;
;     ```json
;     {"a": [1, 2, 3]}
;     ```
;
; The host grammar's whole job is to recognize each fence and hand back two
; spans: the fence's info string (`lang') and the raw bytes between the opening
; and closing fence (`code'). It deliberately treats the code body as opaque
; bytes -- it is a SECOND, separately compiled grammar's job to parse it.
;
; This is the W16-3 multi-parser story, and it is DIFFERENT from std.fence's W62
; injection demo. std.fence parses both languages inside ONE grammar with the
; runtime `(inject grammar: NAME ...)' form, yielding one composite tree. Here
; there is no inject: the host program links TWO independently generated parsers
; (this one and std.json, each emitted with its own `--name' symbol prefix so
; their symbols do not collide), parses the document with the host parser, reads
; the `code' field's byte span off the tree, and runs the json parser on those
; bytes itself. Two parsers, two trees, one binary.
;
; This is OUR grammar; it is authored here, not ported from a third party.

(tinyparse version: "0.1")

(module examples.embed.host)

(grammar host
  start: Document

  ; Whitespace between blocks is trivia. The fence body is matched byte-by-byte
  ; (extras: none on the body rule), so a code block keeps its own whitespace for
  ; the embedded parser.
  (token Space (regex "[ \t\r\n]+"))

  ; The info string after the opening backticks names the embedded language.
  (token Info (regex "[A-Za-z0-9_]+"))

  (extras
    (trivia Space))

  ; A document is one or more fenced blocks, then end of input.
  (rule Document
    (seq (repeat1 Fence) EOF))

  ; A fence: ```<info><newline> CODE <newline>```. The info string and the code
  ; body are each captured in a field so the host program can read their spans
  ; straight off the tree. The body is opaque to this grammar.
  (rule Fence extras: none
    (seq "```"
         (field lang: Info)
         (regex "[ \t]*") "\n"
         (field code: Body)
         "\n" "```"))

  ; The code body is every byte that does not begin the closing fence line. The
  ; closing fence is a newline followed by three backticks, so the body runs up
  ; to (but not including) that terminator.
  (rule Body extras: none
    (repeat (not (seq "\n" "```") any))))
