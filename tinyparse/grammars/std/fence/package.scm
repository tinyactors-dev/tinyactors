; package.scm -- the manifest for the std.fence grammar package (W62).
;
; std.fence is the language-injection demonstrator: a document of Markdown-style
; fenced code blocks whose info string selects the language each block's content
; is parsed by. It injects std.javascript into `js` fences and std.json into
; `json` fences, producing one composite tree with each region parsed by its own
; grammar. This is OUR grammar, authored here, not ported from a third party.

(pocketparse version: "0.1")

(package
  name: std.fence
  version: "0.1.0"
  provides: (Document))
