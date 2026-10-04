; grammar.scm -- std.fence, a tiny fenced-code-block host that injects languages.
;
; This is the W62 language-injection demonstrator: a document is a run of
; Markdown-style fenced code blocks,
;
;     ```js
;     function f(x) { return g(x); }
;     ```
;     ```json
;     {"a": [1, 2, 3]}
;     ```
;
; whose info string after the opening fence selects the language the block's
; CONTENT is parsed by. A `js` fence injects std.javascript over its content; a
; `json` fence injects std.json. The (inject grammar: NAME content: REGION) form
; matches REGION to delimit the embedded span, then re-parses that span with the
; named grammar and splices the result -- so one std.fence parse yields ONE
; composite tree with a JavaScript subtree under the js fence and a JSON subtree
; under the json fence, each node tagged with the language that produced it.
;
; This is OUR grammar; it lives under grammars/std because it is authored and
; owned here, not ported from a third party.

(pocketparse version: "0.1")

(module std.fence)

(grammar fence
  start: Document

  ; Whitespace between blocks is trivia. The fence content itself is matched
  ; byte-by-byte (extras: none on the content rules), so block bodies keep their
  ; own whitespace for the embedded parser.
  (token Space (regex "[ \t\r\n]+"))

  (extras
    (trivia Space))

  ; A document is one or more fenced blocks, then end of input.
  (rule Document
    (seq (repeat1 Block) EOF))

  (rule Block
    (choice JsFence JsonFence))

  ; A js fence: ```js<newline> CONTENT ```. The content is every byte up to the
  ; closing fence, re-parsed as std.javascript and spliced under an injection
  ; node tagged `std.javascript'.
  (rule JsFence extras: none
    (seq "```js" (regex "[ \t]*") "\n"
         (inject grammar: std.javascript content: (field content: FenceBody))
         "\n" "```"))

  ; A json fence: ```json<newline> CONTENT ```. The content is re-parsed as
  ; std.json and spliced under an injection node tagged `std.json'.
  (rule JsonFence extras: none
    (seq "```json" (regex "[ \t]*") "\n"
         (inject grammar: std.json content: (field content: FenceBody))
         "\n" "```"))

  ; The fence body is every byte that does not begin the closing fence line. The
  ; closing fence is a newline followed by three backticks, so the body runs up
  ; to (but not including) that terminator; the trailing newline is consumed as
  ; part of matching the body's last line.
  (rule FenceBody extras: none
    (repeat (not (seq "\n" "```") any))))
