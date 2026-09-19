# Vendored software

`jq/` contains the jq 1.8.2 source release from the jqlang project. Its upstream
license and notices are included in that directory. The repository-level
`mise run vendor-jq` task downloads the release archive, verifies its published
SHA-256 checksum, and reproduces this directory.

`quickjs-ng/` contains the quickjs-ng 0.17.0 source release (the GitHub tag
archive), without its docs, tests, test262 configuration, examples, generated
CLI sources, and git/GitHub metadata. Its upstream `LICENSE` is included in that
directory. The repository-level `mise run vendor-quickjs` task downloads the
archive, verifies its pinned SHA-256 checksum, and reproduces this directory.

`scxml-irp/` contains the W3C SCXML 1.0 Implementation Report Plan test suite,
vendored unmodified. Its README records its license and provenance.
