# Numbers' text and the doubles they read as

`tests/unit/test_cli_decimal.c` reads every file here through
`ta_decimal_read` (`src/cli/decimal.h`, a port of Go's
`strconv.ParseFloat(s, 64)`) and requires each expected double, bit for
bit, or a refusal. A line is `<text> <bits>`, the double's bits in
hexadecimal, or `<text> invalid`: a text that is not a JSON number
(RFC 8259, section 6), or one whose value rounds past the largest double.
A text may hold spaces; the expectation is after the last one. Lines
starting with `#` are comments.

| File | What | Source | Licence |
|---|---|---|---|
| `go-atof.txt` | the `atoftests` table of Go's `src/strconv/atof_test.go` (go1.24.0) whose inputs are JSON numbers (81 of 261: Go also reads hexadecimal, underscores, `Inf`, `NaN` and a leading `+`); the `strings.Repeat` inputs written out; the expected text Go writes converted to its double's bits, a range error to `invalid` | <https://github.com/golang/go/blob/go1.24.0/src/strconv/atof_test.go> | BSD-3-Clause, Copyright 2009 The Go Authors (the notice is in `src/cli/decimal.c`) |
| `hard.txt` | cases picked by hand, a comment above each: halfway cases (to even, and a digit past the 800th that breaks the tie), the largest and smallest normal and subnormal doubles written in full, the halfway between the largest double and 2^1024, exponents past ±400, 1000 digits, and texts that are not JSON numbers | this repository; the bits from Python's `float()`, which rounds correctly | this repository's |
| `values.txt` | the numbers of `tinyactors-test/suites/protocol/values.txt`, written as JSON text | this repository's suite; the bits from Python's `float()` | this repository's |
| `parse-number-fxx-sample.txt` | a seeded sample of Nigel Tao's parse-number-fxx-test-data at commit `55d79b18`: every line of `more-test-cases.txt` and 300 lines of each other data file, each file's lines in their order, its own format (`f16 f32 f64 text`; the test reads the f64 at columns 14 to 30). A text that is not a JSON number (`.5`, `1.`, `0x1p3`, `inf`) must be refused, and so must one whose double is an infinity | <https://github.com/nigeltao/parse-number-fxx-test-data> | Apache-2.0, `parse-number-fxx-LICENSE` |

The round trips are not a file: the test writes a seeded sample of doubles
(random bit patterns, subnormals, integers and short decimals) with
`ta_format_double` and requires each to read back as itself, 20,000 in the
fast pass and two million with `--full`.

The whole parse-number-fxx-test-data corpus (5,268,248 lines, about 200 MB)
is not committed: `mise run json:corpus` fetches it, pinned by commit and
checksum, into `build/parse-number-fxx-test-data/`, and `test_cli_decimal
--full` (`mise run test --full`) reads every file there; without it the full
pass says it skipped them. Every line passed on 2026-11-25.

The fixtures were made once, with scripts kept outside the repository, from
Go's sources and the corpus at the versions above; no test depends on Go,
Python or the network. A deliberate change to the reader regenerates them,
and the diff is reviewed.
