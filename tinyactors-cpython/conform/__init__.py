"""The conformance suite's adapter: `python -m conform` speaks the driver
protocol (tinyactors-test/docs/driver.md) over the public `tinyactors`
module, and frames its calls with the module's own CBOR functions,
`tinyactors.encode` and `tinyactors.decode` (AGENTS.md, "The module's
CBOR"). Not part of the wheel."""
