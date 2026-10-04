#!/usr/bin/env python3
# gen-corpus-suite.py -- turn the vendored tree-sitter JavaScript corpus into a
# committed conformance suite.
#
# For every "==== title ==== / input / --- / tree" case in
# reference/tree-sitter-javascript/test/corpus/*.txt this writes:
#
#   test/suite/jsfamily/inputs/<file>__<n>.js   the case's input bytes, verbatim
#   test/suite/jsfamily/<file>__<n>.test        a harness test that runs
#       `pocketparse run std.javascript --input <that file> --dump-tree`
#       and asserts exit 0 with no ERROR / no MISSING in the dumped tree.
#
# The committed .js fixtures make the suite reproducible in a fresh container,
# where reference/ is absent. Run from tools/pocketparse:
#
#   python3 test/suite/jsfamily/gen-corpus-suite.py
import os, re, glob, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PP_DIR = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
CORPUS = os.path.join(PP_DIR, "reference", "tree-sitter-javascript",
                      "test", "corpus")
INPUTS = os.path.join(HERE, "inputs")

if not os.path.isdir(CORPUS):
    sys.stderr.write("corpus not found at %s\n" % CORPUS)
    sys.exit(1)

os.makedirs(INPUTS, exist_ok=True)

TEST_TEMPLATE = """\
<<<cmd
"$PP" run std.javascript --input test/suite/jsfamily/inputs/%s --dump-tree
<<<assert
exit 0
out-absent ERROR
out-absent MISSING
"""

written = []
for path in sorted(glob.glob(os.path.join(CORPUS, "*.txt"))):
    txt = open(path, encoding="utf-8").read()
    base = os.path.basename(path).replace(".txt", "")
    pat = re.compile(r'(?m)^={3,}[ \t]*\n(.*?)\n={3,}[ \t]*\n')
    parts = pat.split(txt)
    n = 0
    for i in range(1, len(parts), 2):
        body = parts[i + 1]
        m = re.search(r'(?m)^-{3,}[ \t]*\n', body)
        if not m:
            continue
        inp = body[:m.start()]
        if inp.startswith("\n"):
            inp = inp[1:]
        inp = inp.rstrip("\n") + "\n"
        n += 1
        stem = "%s__%d" % (base, n)
        js = os.path.join(INPUTS, stem + ".js")
        with open(js, "w", encoding="utf-8") as f:
            f.write(inp)
        test = os.path.join(HERE, stem + ".test")
        with open(test, "w", encoding="utf-8") as f:
            f.write(TEST_TEMPLATE % (stem + ".js"))
        written.append(stem)

print("wrote %d corpus cases" % len(written))
