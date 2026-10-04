; package.scm — std.shell
; The POSIX shell command language (POSIX sh), as a pocketparse standard grammar.
; Ported (the LANGUAGE, not the code) from the build-time reference under
; tools/pocketparse/reference/tree-sitter-bash/. No third-party code is copied;
; the reference is studied for the language, and its corpus seeds our tests.
;
; The grammar covers the POSIX sh command language in full: simple commands
; (quoted/concatenated words, variable-assignment prefixes, file redirections),
; pipelines, and-or lists, the compound commands (if / for / while / until /
; case / subshell / brace group), function definitions, every word expansion
; ($var ${…} $(…) `…` $((…))), and here-documents (<< and <<-).
;
; What it deliberately does NOT parse — bash-only extensions that are out of
; POSIX scope, and a few known std.shell limitations — is recorded with rationale
; in tools/pocketparse/docs/shell-corpus-scope.md. The conformance corpus under
; test/suite/shellfamily/ imports only the POSIX-parseable sections of the
; build-time tree-sitter-bash reference; the excluded sections are listed there.

(pocketparse version: "0.1")

(package
  name: std.shell
  version: "0.1.0"
  provides: (Program))
