#!/usr/bin/env python3
"""Check a language's translation of the W3C suite and the scenarios.

Every document converted for <language> (the W3C tests in <w3c-dir>, as
listed by its index.txt, and every .scxml under <scenarios-dir>) must be
well-formed XML. In every document but those of the W3C tests the skip
files name (tests that cannot be translated, with their reasons):

- no conf: element or attribute is left;
- for a language with a compile check (CHECKS), the document is in
  <language> (or the null datamodel), and every expression, location and
  script in it compiles, except the values the stylesheet writes for the
  conf: attributes that are meant to be invalid (a probe document,
  translated by <stylesheet>, finds them).

Usage: uv run --with saxonche suites/w3c/check.py <language> <stylesheet>
           <w3c-dir> <scenarios-dir> [--skip <file>]...
"""
import pathlib
import sys
import xml.etree.ElementTree as ET

from saxonche import PySaxonProcessor

SCXML = "{http://www.w3.org/2005/07/scxml}"
CONF = "{http://www.w3.org/2005/scxml-conformance}"

# The conf: attributes whose translation may be invalid in a language: an
# item that is no variable name, a namelist that names none, a value that
# cannot be assigned, a cond that cannot be read as a boolean.
PROBE = """<scxml xmlns="http://www.w3.org/2005/07/scxml"
    xmlns:conf="http://www.w3.org/2005/scxml-conformance" version="1.0">
  <foreach conf:illegalItem=""/>
  <send event="e" conf:invalidNamelist=""/>
  <assign conf:illegalExpr=""/>
  <transition conf:nonBoolean=""/>
</scxml>"""

# Attributes whose value is an expression, and those whose value is a
# location (assigned to; <param location> is read as well).
EXPRESSIONS = ("cond", "expr", "targetexpr", "eventexpr", "typeexpr", "delayexpr", "srcexpr",
               "sendidexpr", "array")
LOCATIONS = ("location", "idlocation", "item", "index")


def python(attribute, value):
    """(kind, source, mode): the compile() calls a value must pass in Python."""
    if attribute == "script":
        return [("script", value, "exec")]
    if attribute in EXPRESSIONS:
        return [("expression", value, "eval")]
    if attribute in LOCATIONS:
        read = [("expression", value, "eval")] if attribute == "location" else []
        return [("location", f"{value} = None", "exec")] + read
    if attribute == "namelist":
        return [("expression", name, "eval") for name in value.split()]
    return []


def python_compile(source, mode):
    try:
        compile(source, "<chart>", mode)
        return None
    except SyntaxError as error:
        return f"{type(error).__name__}: {error.msg}"


# The compile check of each language that has one.
CHECKS = {"python": (python, python_compile)}


def probe(stylesheet):
    """The (attribute, value) pairs the stylesheet writes for the PROBE."""
    with PySaxonProcessor(license=False) as proc:
        exe = proc.new_xslt30_processor().compile_stylesheet(stylesheet_file=str(stylesheet))
        out = ET.fromstring(exe.transform_to_string(xdm_node=proc.parse_xml(xml_text=PROBE)))
    return {(name, value) for element in out.iter() for name, value in element.attrib.items()
            if name in ("item", "namelist", "expr", "cond")}


def w3c_documents(w3c):
    """(test id, document) for every chart of every test in the index."""
    for line in (w3c / "index.txt").read_text().splitlines():
        test, _conformance, start, deps = line.split(" ")
        for rel in [start] + ([] if deps == "-" else deps.split(",")):
            if rel.endswith(".scxml"):
                yield test, rel


def untranslated(root):
    """The conf: elements and attributes the stylesheet left in a document."""
    for element in root.iter():
        if element.tag.startswith(CONF):
            yield f"<conf:{element.tag[len(CONF):]}>"
        yield from (f"conf:{name[len(CONF):]}" for name in element.attrib if name.startswith(CONF))


def values(root, language):
    """(element, attribute, value) of every attribute and script in the
    parts of the document whose datamodel is `language` (charts inline in
    <content> included)."""
    def walk(element, datamodel):
        if element.tag == SCXML + "scxml":
            datamodel = element.get("datamodel", datamodel)
        if datamodel == language:
            for name, value in element.attrib.items():
                yield element, name, value
            if element.tag == SCXML + "script" and element.get("src") is None:
                yield element, "script", element.text or ""
        for child in element:
            yield from walk(child, datamodel)
    yield from walk(root, None)


def main(argv):
    args, skips = [], []
    rest = iter(argv[1:])
    for arg in rest:
        if arg == "--skip":
            skips.append(pathlib.Path(next(rest)))
        else:
            args.append(arg)
    if len(args) != 4:
        print(__doc__)
        return 2
    language, stylesheet, w3c, scenarios = args[0], *map(pathlib.Path, args[1:])
    skipped = {line.split(" ", 1)[0] for path in skips
               for line in path.read_text().splitlines() if line.strip() and not line.startswith("#")}
    check = CHECKS.get(language)
    invalid = probe(stylesheet)

    tests = {test for test, _rel in w3c_documents(w3c)}
    documents = [(f"w3c {test} {rel}", w3c / rel, test not in skipped) for test, rel in w3c_documents(w3c)]
    documents += [(f"scenario {path.relative_to(scenarios)}", path, True) for path in sorted(scenarios.rglob("*.scxml"))]

    failures, counts, null = [], {}, []
    for name, path, checked in documents:
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError as error:
            failures.append(f"{name}: not well-formed XML: {error}")
            continue
        if not checked:
            continue
        failures.extend(f"{name}: {item} is not translated" for item in untranslated(root))
        if check is None:
            continue
        datamodel = root.get("datamodel")
        if datamodel == "null":
            null.append(name)
        elif datamodel != language:
            failures.append(f"{name}: datamodel {datamodel!r}, not {language!r}: skip the test with a reason")
        kinds, compile_ = check
        for element, attribute, value in values(root, language):
            if (attribute, value) in invalid:
                continue
            for kind, source, mode in kinds(attribute, value):
                counts[kind] = counts.get(kind, 0) + 1
                error = compile_(source, mode)
                if error:
                    failures.append(f"{name}: <{element.tag.replace(SCXML, '')} {attribute}={value!r}>: {error}")

    for failure in failures:
        print(f"FAIL {failure}")
    scenario_count = sum(1 for name, _path, _checked in documents if name.startswith("scenario "))
    summary = (f"check {language}: {len(tests)} W3C tests ({len(tests & skipped)} skipped) and "
               f"{scenario_count} scenario documents, {len(documents)} documents well-formed")
    if check is not None:
        compiled = ", ".join(f"{counts.get(kind, 0)} {kind}s" for kind in ("expression", "location", "script"))
        meant = ", ".join(sorted({repr(value) for _attribute, value in invalid}))
        summary += f"; compiled {compiled}; not compiled, meant to be invalid: {meant}"
        if null:
            summary += f"; in the null datamodel: {', '.join(null)}"
    print(f"{summary}; {len(failures)} failures" if failures else summary)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
