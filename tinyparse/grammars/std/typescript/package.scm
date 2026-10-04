; package.scm — the manifest for the std.typescript grammar package (Phase 8).
;
; std.typescript is the JavaScript family with the type system turned on and JSX
; turned off, exactly the parameterisation tree-sitter-typescript's `typescript`
; dialect uses. The grammar.scm beside this manifest `(extends std.javascript)`
; and layers the TypeScript type system on top: type annotations, generics,
; interfaces, type aliases, enums, namespaces, the structural type grammar, and
; the `as`/`satisfies`/type-assertion expression forms.
;
; The `types` parameter is on so the type productions compile in; `jsx` is off so
; a `<T>` in expression position is a type assertion / type-argument list, not a
; JSX element (the well-known TS-vs-TSX ambiguity, resolved here by parameter).
; std.tsx extends THIS package and turns jsx back on.
;
; This is OUR grammar, authored here from a build-time porting reference under
; tools/tinyparse/reference/tree-sitter-typescript/. No third-party code is
; copied into the package; only the language is ported.

(tinyparse version: "0.1")

(package
  name: std.typescript
  version: "0.1.0"
  extends: (std.javascript)
  params: (types: #t jsx: #f)
  provides: (program))
