; package.scm — the manifest for the std.javascript grammar package (Phase 8,
; W38). A package is a directory the compiler resolves by name and version; this
; manifest names it, pins a version, and declares the feature parameters the
; family accepts. As in tree-sitter-javascript, JSX is part of the base grammar
; (the corpus exercises it unconditionally), so `jsx` is on by default; `types`
; stays off, for the TS/TSX extensions to turn on later via
; (extends (std.javascript types: #t)). The sibling grammar.scm is the source
; the resolver compiles with these parameters in scope.
;
; This is OUR grammar, authored here from a build-time porting reference under
; tools/pocketparse/reference/tree-sitter-javascript/. No third-party code is
; copied into the package; only the language is ported.

(pocketparse version: "0.1")

(package
  name: std.javascript
  version: "0.1.0"
  params: (jsx: #t types: #f)
  provides: (program))
