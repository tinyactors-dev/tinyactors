# W3C SCXML 1.0 Implementation Report Plan test suite

This directory is an unmodified copy of the W3C SCXML 1.0 conformance tests
from <https://www.w3.org/Voice/2013/scxml-irp/> (version of 10 March 2015,
contributor Jim Barnett). The suite is final and will not change, so it is
vendored directly rather than downloaded by a task.

## License

The test suite is licensed under both the
[W3C Test Suite License](https://www.w3.org/Consortium/Legal/2008/04-testsuite-license)
and the
[W3C 3-clause BSD License](https://www.w3.org/Consortium/Legal/2008/03-bsd-license).
`index.html` is the W3C page that states this.

## Contents

- `manifest.xml`: one `<assert>` per tested assertion, with its spec section,
  `conformance` (`mandatory` or `optional`), `manual`, start files, and
  dependencies.
- `NNN/testNNN.txml`: 200 tests, one directory per assertion. Test 403 has
  three start files. `testNNNsub1.txml` and `testNNN.txt` files are
  dependencies that tests load through `src`.
- `confEcma.xsl`, `confXPath.xsl`: the W3C transforms from the
  datamodel-neutral `conf:` markup to the ECMAScript and XPath data models.
- `index.html`: the Implementation Report Plan page.

A test passes when the state machine reaches the final state `pass`
(`<conf:pass/>`), and fails when it reaches `fail`.

## Provenance

The W3C server rate-limits downloads. These 25 files were taken from the
uSCXML project's copy of the suite
(<https://github.com/tklab-tud/uscxml/tree/master/test/w3c/txml>):

```
527/test527.txml 530/test530.txml 533/test533.txml 534/test534.txml
550/test550.txml 551/test551.txml 552/test552.txml 552/test552.txt
553/test553.txml 554/test554.txml 557/test557.txml 557/test557.txt
558/test558.txml 558/test558.txt 560/test560.txml 561/test561.txml
562/test562.txml 567/test567.txml 569/test569.txml 570/test570.txml
576/test576.txml 577/test577.txml 578/test578.txml 579/test579.txml
580/test580.txml
```

Every other file came from the W3C server. A sample of those files is
byte-identical to the uSCXML copy, and uSCXML's `confEcma.xsl` is identical to
the W3C one.

## Use in this repository

`mise run import-scxml-irp` (in `tinyactors-test`) converts these tests into
chart and scenario files for the jq data model; see
`tinyactors-test/ruby/scxml_irp/README.md`.
