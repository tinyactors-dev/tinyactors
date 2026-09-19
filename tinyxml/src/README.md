Implementation modules go here.

The planned split (see ../docs/internal/):

- arena.{c,h}      linked-chunk arena and marks
- text.{c,h}       UTF-8 validation, XML Char checks, byte scans, str helpers
- staging.{c,h}    the padded input window and feed accounting
- lexer.{c,h}      delimiter scanning per lexer state
- names.{c,h}      namespace and name interning
- columns.{c,h}    paged vectors over the arena
- doc.{c,h}        document assembly, end patching, mark/rewind
- cursor.{c,h}     navigation and range queries
- parser.{c,h}     feed/step state machine and error reporting
- writer.{c,h}     event writer, escaping, resumable output
- internal.h       umbrella listing the private headers in dependency order
