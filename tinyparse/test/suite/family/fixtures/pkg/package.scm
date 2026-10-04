; package.scm — the manifest for the `family.demo` package fixture (W29).
;
; A package is a directory the compiler resolves by name, version, and feature
; set. This manifest names the package, pins a version, and declares the `feature`
; parameter it accepts (bound on by default here). The sibling grammar.scm is the
; grammar source the resolver compiles with those parameters in scope.

(package
  name: family.demo
  version: "0.1.0"
  params: (feature: #t)
  provides: (Source))
