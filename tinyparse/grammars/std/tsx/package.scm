; package.scm — the manifest for the std.tsx grammar package (Phase 8).
;
; std.tsx is std.typescript with JSX turned back on: the TypeScript type system
; AND the JSX productions the JavaScript base already carries. It is the
; tree-sitter-typescript `tsx` dialect. The grammar.scm beside this manifest
; `(extends std.typescript)` and re-enables the JSX expression productions that
; std.typescript suppressed (jsx: #f), resolving the `<T>` ambiguity the tsx way:
; a leading `<Name …>` is a JSX element, and a type cast is written `x as T`.
;
; This is OUR grammar, authored here from a build-time porting reference under
; tools/pocketparse/reference/tree-sitter-typescript/. No third-party code is
; copied into the package; only the language is ported.

(pocketparse version: "0.1")

(package
  name: std.tsx
  version: "0.1.0"
  extends: (std.typescript)
  params: (types: #t jsx: #t)
  provides: (program))
