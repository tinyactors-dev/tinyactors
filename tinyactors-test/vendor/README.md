# Vendored software

`jq/` contains the jq 1.8.2 source release from the jqlang project. Its upstream
license and notices are included in that directory. The repository-level
`mise run vendor-jq` task downloads the release archive, verifies its published
SHA-256 checksum, and reproduces this directory.
