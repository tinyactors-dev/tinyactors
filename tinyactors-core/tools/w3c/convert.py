#!/usr/bin/env python3
"""Instantiate W3C IRP .txml templates for the ECMAScript datamodel.

The W3C stylesheets are XSLT 2.0 (xsl:analyze-string), so this uses SaxonC
(saxonche); libxslt-based processors silently produce empty conditions.

Reads <irp-dir>/manifest.xml, keeps automatic (manual="false") tests,
applies confEcma.xsl to every start/dep .txml and writes .scxml files plus
an index ("<id> <conformance> <start.scxml> <deps|->" per line) into <out-dir>.

Usage: uv run --with saxonche tools/w3c/convert.py <irp-dir> <out-dir>
"""
import pathlib
import sys
import xml.etree.ElementTree as ET

from saxonche import PySaxonProcessor


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    irp, out = pathlib.Path(argv[1]).resolve(), pathlib.Path(argv[2]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    manifest = ET.parse(irp / "manifest.xml")
    index = []
    with PySaxonProcessor(license=False) as proc:
        xslt = proc.new_xslt30_processor()
        exe = xslt.compile_stylesheet(stylesheet_file=str(irp / "confEcma.xsl"))
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
            deps = [str(pathlib.Path(d).with_suffix(".scxml")) for d in files[1:] if d.endswith(".txml")]
            index.append(f"{tid} {conf} {pathlib.Path(start).with_suffix('.scxml')} {','.join(deps) or '-'}")
    (out / "index.txt").write_text("\n".join(index) + "\n")
    print(f"converted {len(index)} tests into {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
