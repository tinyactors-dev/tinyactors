#!/usr/bin/env python3
# gen-corpus-suite.py -- turn the vendored tree-sitter TypeScript corpus into a
# committed conformance suite, the TS-family sibling of jsfamily's extractor.
#
# A tree-sitter case is "==== title [directive] ==== / input / --- / tree". The
# directive line `:language(tsx)` (or `:language(typescript)`) selects which
# dialect tree-sitter parses the case with; absent, the dialect is typescript.
# We honour it: a tsx case runs against std.tsx, every other case against
# std.typescript. The input extension follows the dialect (.tsx vs .ts).
#
# For every case in reference/tree-sitter-typescript/test/corpus/*.txt this writes:
#
#   test/suite/tsfamily/inputs/<file>__<n>.{ts,tsx}  the case's input bytes
#   test/suite/tsfamily/<file>__<n>.test             a harness test that runs
#       `tinyparse run std.<dialect> --input <that file> --dump-tree`
#       and asserts exit 0 with no ERROR / no MISSING in the dumped tree.
#
# Every case in the vendored corpus is VALID TypeScript (none of the expected
# trees carries an (ERROR) or (MISSING) node), so every test is an accept test.
#
# The committed fixtures make the suite reproducible in a fresh container, where
# reference/ is absent. Run from tools/tinyparse:
#
#   python3 test/suite/tsfamily/gen-corpus-suite.py
#
# Hand-written tests whose stem does not match a corpus file are left untouched:
# `attach_leading__1.{ts,test}' is an authored backtracking-stress case for the
# `attach: leading' comment policy (docs/comment-attachment.md), not derived from the
# vendored corpus, so this generator neither writes nor clobbers it.
import os, re, glob, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TP_DIR = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
CORPUS = os.path.join(TP_DIR, "reference", "tree-sitter-typescript",
                      "test", "corpus")
INPUTS = os.path.join(HERE, "inputs")

if not os.path.isdir(CORPUS):
    sys.stderr.write("corpus not found at %s\n" % CORPUS)
    sys.exit(1)

os.makedirs(INPUTS, exist_ok=True)

TEST_TEMPLATE = """\
<<<cmd
"$TP" run std.%s --input test/suite/tsfamily/inputs/%s --dump-tree
<<<assert
exit 0
out-absent ERROR
out-absent MISSING
"""

# Structural assertions, keyed by stem. The accept-only check above (exit 0, no
# ERROR, no MISSING) is necessary but not sufficient: a grammar can parse the
# bytes cleanly yet build the WRONG tree -- e.g. read a `function f() {}` as a
# bodyless `function_signature` followed by a free-standing `statement_block`,
# which carries no ERROR but loses the declaration. Cases where the right tree
# turns on a parser decision (signature-vs-declaration, an `asserts`/predicate
# return, an `import(...)` in type position) carry extra assertions on the count
# of the deciding node types, so a regression to a degenerate-but-clean parse is
# caught here rather than silently passing. The counts mirror the tree-sitter
# expected tree in the vendored corpus.
EXTRA_ASSERTS = {
    "functions__1":  ["out-count 5 function_declaration", "out-absent function_signature"],
    "functions__9":  ["out-count 2 function_declaration", "out-count 4 function_signature"],
    "types__7":      ["out-count 2 function_declaration", "out-absent function_signature"],
    "types__34":     ["out-count 1 function_declaration", "out-absent function_signature"],
    "types__35":     ["out-count 2 function_declaration", "out-absent function_signature"],
    "types__36":     ["out-count 2 ambient_declaration", "out-count 4 asserts",
                      "out-absent expression_statement"],
    "types__37":     ["out-count 2 function_declaration", "out-absent function_signature"],
    "types__50":     ["out-contains import_type", "out-count 1 type_alias_declaration",
                      "out-absent expression_statement"],
    # attach: leading -- a doc comment binds to the FOLLOWING statement as a sibling
    # in the parent, never a child of the declaration it sits below. The dump indents
    # a direct child of `program' by exactly two spaces and a child of a declaration by
    # four, so an anchored `^  line_comment [span]' regex asserts the comment is a
    # program-level sibling at its known span; a regression to child placement indents
    # it four spaces and the two-space-anchored regex stops matching. (A leading-space
    # out-absent cannot express this -- the harness strips an assertion's leading
    # whitespace.) The `^  <node> [start-end]' regex on the PRECEDING declaration, plus
    # the out-absent on its old over-extended span, assert its extent is TRIMMED to its
    # last real token (it does NOT run across the floated comment and overlap it as a
    # sibling). See docs/comment-attachment.md.
    "types__39":     [r"out-regex ^  line_comment \[74-114\]",
                      r"out-regex ^  line_comment \[141-160\]",
                      r"out-regex ^  type_alias_declaration \[44-74\]",
                      r"out-regex ^  type_alias_declaration \[115-141\]",
                      "out-absent type_alias_declaration [44-115]",
                      "out-absent type_alias_declaration [115-161]"],
    "functions__10": [r"out-regex ^  line_comment \[17-40\]",
                      r"out-regex ^  function_signature \[0-17\]",
                      "out-absent function_signature [0-41]"],
}

# Header block: the title and any directive line(s) live between two fences.
HEADER = re.compile(r'(?ms)^={3,}[ \t]*\n(.*?)\n={3,}[ \t]*\n')
SEP = re.compile(r'(?m)^-{3,}[ \t]*\n')

written = []
for path in sorted(glob.glob(os.path.join(CORPUS, "*.txt"))):
    txt = open(path, encoding="utf-8").read()
    base = os.path.basename(path).replace(".txt", "")
    parts = HEADER.split(txt)
    n = 0
    for i in range(1, len(parts), 2):
        header = parts[i]
        body = parts[i + 1]
        m = SEP.search(body)
        if not m:
            continue
        # Dialect from a :language(...) directive in the header block.
        dm = re.search(r':language\(([a-z]+)\)', header)
        dialect = dm.group(1) if dm else "typescript"
        if dialect not in ("typescript", "tsx"):
            dialect = "typescript"
        ext = "tsx" if dialect == "tsx" else "ts"
        inp = body[:m.start()]
        if inp.startswith("\n"):
            inp = inp[1:]
        inp = inp.rstrip("\n") + "\n"
        n += 1
        stem = "%s__%d" % (base, n)
        srcfile = os.path.join(INPUTS, stem + "." + ext)
        with open(srcfile, "w", encoding="utf-8") as f:
            f.write(inp)
        test = os.path.join(HERE, stem + ".test")
        body_text = TEST_TEMPLATE % (dialect, stem + "." + ext)
        for extra in EXTRA_ASSERTS.get(stem, []):
            body_text += extra + "\n"
        with open(test, "w", encoding="utf-8") as f:
            f.write(body_text)
        written.append(stem)

print("wrote %d corpus cases" % len(written))
