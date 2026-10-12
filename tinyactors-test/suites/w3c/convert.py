#!/usr/bin/env python3
"""Instantiate .txml templates in the W3C IRP notation with a W3C stylesheet.

The W3C stylesheets are XSLT 2.0 (xsl:analyze-string), so this uses SaxonC
(saxonche); libxslt-based processors silently produce empty conditions.

The W3C suite: reads <irp-dir>/manifest.xml, keeps automatic
(manual="false") tests, applies <stylesheet> (one language's: W3C's
confEcma.xsl, our confPython.xsl) to every start/dep .txml and writes
.scxml files plus an index ("<id> <conformance> <start.scxml>
<deps|->" per line) into <out-dir>. The deps are every <dep> of the
manifest: charts (.txml, as .scxml) and the files a src="" reads (.txt).

The scenarios (--scenarios): applies <stylesheet> to every .txml under
<src-dir> and writes it as .scxml at the same place under <out-dir>.

<out-dir> must be new or empty: the conversion is written into a fresh
directory, which lib/w3c.rb (W3C.publish) then puts in place with one
rename, so that a run reading the previous conversion never sees half of
this one. Writing over an earlier conversion is refused.

Usage: uv run --with saxonche suites/w3c/convert.py <stylesheet> <irp-dir> <out-dir>
       uv run --with saxonche suites/w3c/convert.py --scenarios <stylesheet> <src-dir> <out-dir>
"""
import pathlib
import sys
import xml.etree.ElementTree as ET


def w3c(proc, stylesheet, irp, out):
    xslt = proc.new_xslt30_processor()
    exe = xslt.compile_stylesheet(stylesheet_file=str(stylesheet))
    manifest = ET.parse(irp / "manifest.xml")
    index = []
    for test in manifest.iter("test"):
        if test.get("manual") == "true":
            continue
        tid, conf = test.get("id"), test.get("conformance")
        start = test.find("start").get("uri")
        files = [start] + [d.get("uri") for d in test.findall("dep")]
        if not all((irp / rel).exists() for rel in files):
            continue
        for rel in files:
            src, dst = irp / rel, out / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if rel.endswith(".txml"):
                res = exe.transform_to_string(source_file=str(src))
                dst.with_suffix(".scxml").write_text(res)
            else:
                dst.write_bytes(src.read_bytes())
        deps = [str(pathlib.Path(d).with_suffix(".scxml")) if d.endswith(".txml") else d for d in files[1:]]
        index.append(f"{tid} {conf} {pathlib.Path(start).with_suffix('.scxml')} {','.join(deps) or '-'}")
    (out / "index.txt").write_text("\n".join(index) + "\n")
    print(f"converted {len(index)} tests into {out}")


def scenarios(proc, stylesheet, src, out):
    xslt = proc.new_xslt30_processor()
    exe = xslt.compile_stylesheet(stylesheet_file=str(stylesheet))
    count = 0
    for template in sorted(src.rglob("*.txml")):
        dst = (out / template.relative_to(src)).with_suffix(".scxml")
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(exe.transform_to_string(source_file=str(template)))
        count += 1
    print(f"converted {count} scenario documents into {out}")


def fresh(out):
    """Creates <out-dir>, or takes it empty; False when it holds files."""
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        print(f"convert.py: {out} is not empty: convert into a new directory (lib/w3c.rb, W3C.publish)",
              file=sys.stderr)
        return False
    return True


def main(argv):
    paths = [pathlib.Path(arg).resolve() for arg in argv[1:] if arg != "--scenarios"]
    if len(paths) == 3 and not fresh(paths[2]):
        return 1
    from saxonche import PySaxonProcessor  # imported here: refusing an out-dir needs no SaxonC

    if "--scenarios" in argv[1:] and len(paths) == 3:
        with PySaxonProcessor(license=False) as proc:
            scenarios(proc, *paths)
        return 0
    if "--scenarios" not in argv[1:] and len(paths) == 3:
        with PySaxonProcessor(license=False) as proc:
            w3c(proc, *paths)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
