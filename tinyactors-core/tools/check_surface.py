#!/usr/bin/env python3
"""Binding surface gate: every public C function is classified, and every
function classified "bind" is reachable from every binding.

bindings/surface.json:
  "bindings":  { name: [source globs] }   C sources that make up a binding
               (its glue plus library layers it calls, e.g. src/host/*.c)
  "functions": { ta_name: "bind" | { "skip": "reason" } |
                 { "bind": ["node"], "skip": "reason for the others" } }

Classification is a manifest check: the TA_API declarations of
include/tinyactors/*.h against surface.json [artifact].

Reachability is decided by the linker, not by names in the sources: each
binding's sources plus the library layers (src/{base,image,core,system,
compile}) are compiled without optimization and linked with dead-code
elimination from the binding's entry points — the node addon with the host
C compiler from its module registration (napi_register_module_v1), the web
binding with zig for wasm32-wasi (a reactor: its exports are the roots).
A function counts as reachable when it survives that link: some path of
calls (or function pointers) from the binding's entry points reaches it.

The JavaScript layer is checked the same way one level up, by running it:
every export of each binding as loaded at runtime (bindings/node,
bindings/web/node.mjs) and every public method, getter and static of its
System, Actor and SystemImage classes must be declared in that binding's
index.d.ts ("typings" in surface.json): the TypeScript compiler checks a
generated file that refers to each of them, so the TypeScript surface cannot
fall behind the JavaScript one. Needs the bindings built (`mise run
node:test`, `mise run web:build`), as check_layers.py does before.

Exit 1 lists unclassified, stale and missing functions; prints a coverage
table either way.
"""
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "build/surface"
LIBS = ["src/base/*.c", "src/image/*.c", "src/trace/*.c", "src/core/*.c", "src/system/*.c", "src/compile/*.c",
        "src/otlp/*.c"]
INCLUDES = ["include", "src"]
CLASSES = ("System", "Actor", "SystemImage")
# the module each typings file describes (loaded by node to read the runtime surface)
RUNTIME = {"bindings/node/index.d.ts": "bindings/node/index.js", "bindings/web/index.d.ts": "bindings/web/node.mjs"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def public_functions():
    out = {}
    for h in sorted((ROOT / "include/tinyactors").glob("*.h")):
        for name in re.findall(r"TA_API\s+[^;{(]*?\b(ta_\w+)\s*\(", strip_comments(h.read_text())):
            out[name] = h.name
    return out


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def wasm_function_names(path):
    """function names from a wasm module's "name" section"""
    b = path.read_bytes()

    def leb(i):
        r = s = 0
        while True:
            x = b[i]
            i += 1
            r |= (x & 0x7F) << s
            s += 7
            if x < 0x80:
                return r, i

    names, i = set(), 8
    while i < len(b):
        sid = b[i]
        n, i = leb(i + 1)
        end = i + n
        if sid == 0:
            ln, j = leb(i)
            if b[j:j + ln] == b"name":
                j += ln
                while j < end:
                    sub = b[j]
                    sl, j = leb(j + 1)
                    if sub == 1:
                        cnt, k = leb(j)
                        for _ in range(cnt):
                            _, k = leb(k)
                            l2, k = leb(k)
                            names.add(b[k:k + l2].decode())
                            k += l2
                    j += sl
        i = end
    return names


def link_node(sources):
    inc = run(["node", "-p", "require('path').resolve(process.execPath, '../../include/node')"]).stdout.strip()
    out = OUT / "node.out"
    r = run(["cc", "-O0", "-std=c11", "-ffunction-sections", "-fdata-sections", *[f"-I{i}" for i in INCLUDES],
             f"-I{inc}", "-DNAPI_VERSION=8", "-DNODE_GYP_MODULE_NAME=tinyactors", "-nostdlib", "-static",
             "-Wl,-e,napi_register_module_v1", "-Wl,--gc-sections", "-Wl,--unresolved-symbols=ignore-all",
             *sources, "-o", str(out)])
    if r.returncode:
        return None, r.stderr
    nm = run(["nm", "--defined-only", str(out)]).stdout
    return {m.group(1) for m in re.finditer(r"^\S+ [Tt] (\w+)$", nm, re.M)}, ""


def link_web(sources):
    out = OUT / "web.wasm"
    r = run(["zig", "cc", "-target", "wasm32-wasi", "-mexec-model=reactor", "-O0", "-std=c11",
             *[f"-I{i}" for i in INCLUDES], "-Ibindings/web/src", *sources, "-o", str(out)])
    if r.returncode:
        return None, r.stderr
    return wasm_function_names(out), ""


LINKERS = {"node": link_node, "web": link_web}


def runtime_surface(module):
    """{exports: [...], classes: {name: [members]}} of a binding, by loading it"""
    js = r"""
const file = process.argv[1];
const m = file.endsWith('.mjs') ? await import(require('url').pathToFileURL(file)) : { default: require(file) };
const named = Object.keys(m).filter((k) => k !== 'default');
const api = named.length ? m : m.default;
const pub = (n) => /^[a-zA-Z]/.test(n);
const out = { exports: (named.length ? named : Object.keys(api)).sort(), classes: {} };
for (const c of process.argv.slice(2)) {
  const C = api[c];
  if (typeof C !== 'function') continue;
  out.classes[c] = {
    proto: Object.getOwnPropertyNames(C.prototype).filter((n) => n !== 'constructor' && pub(n)).sort(),
    static: Object.getOwnPropertyNames(C).filter((n) => !['length', 'name', 'prototype'].includes(n) && pub(n)).sort(),
  };
}
console.log(JSON.stringify(out));
setImmediate(() => process.exit(0));
"""
    r = run(["node", "--input-type=commonjs", "-e", f"(async () => {{ {js} }})()", str(ROOT / module), *CLASSES],
            timeout=120)
    if r.returncode:
        return None, r.stderr.strip().splitlines()[-1:] or ["?"]
    return json.loads(r.stdout.strip().splitlines()[-1]), None


def check_typings(manifest, problems):
    internal = set(manifest.get("internal", []))
    tsc = ROOT / "bindings/node/node_modules/.bin/tsc"
    if not tsc.exists():
        problems.append("typings: bindings/node/node_modules/.bin/tsc missing (run `mise run node:test` first)")
        return
    for dts in manifest.get("typings", []):
        surface, err = runtime_surface(RUNTIME[dts])
        if surface is None:
            problems.append(f"{dts}: cannot load {RUNTIME[dts]} to read its runtime surface (build it first): {err[0]}")
            continue
        lines = [f"import * as ta from '{(ROOT / dts).with_suffix('').with_suffix('')}';"]
        what = {}

        def ref(expr, desc):
            what[len(lines) + 1] = desc
            lines.append(f"type T{len(lines)} = {expr};")

        for name in surface["exports"]:
            if name not in internal:
                ref(f"typeof ta.{name}", f"export {name}")
        for cls, members in surface["classes"].items():
            for n in members["proto"]:
                if f"{cls}.{n}" not in internal:
                    ref(f"InstanceType<typeof ta.{cls}>['{n}']", f"{cls}.{n}")
            for n in members["static"]:
                if f"{cls}.{n}" not in internal:
                    ref(f"(typeof ta.{cls})['{n}']", f"{cls}.{n} (static)")
        OUT.mkdir(parents=True, exist_ok=True)
        check = OUT / (dts.replace("/", "_") + ".check.ts")
        check.write_text("\n".join(lines) + "\n")
        r = run([str(tsc), "--noEmit", "--strict", "--target", "ES2022", "--module", "commonjs",
                 "--moduleResolution", "node10", "--lib", "ES2022,DOM", str(check)], timeout=300)
        for m in re.finditer(r"\.check\.ts\((\d+),\d+\): error TS\d+: (.*)", r.stdout):
            problems.append(f"{dts}: {what.get(int(m.group(1)), 'line ' + m.group(1))} is not declared ({m.group(2)})")
        if r.returncode and not re.search(r"\.check\.ts\(", r.stdout):
            problems.append(f"{dts}: tsc failed:\n" + r.stdout[-800:])


def main():
    manifest = json.loads((ROOT / "bindings/surface.json").read_text())
    funcs = public_functions()
    classes = manifest["functions"]
    problems = []
    for name in sorted(set(funcs) - set(classes)):
        problems.append(f"unclassified: {name} ({funcs[name]}) — add it to bindings/surface.json")
    for name in sorted(set(classes) - set(funcs)):
        problems.append(f"stale: {name} is classified but no longer public")
    OUT.mkdir(parents=True, exist_ok=True)
    linked = {}
    libs = [f for g in LIBS for f in sorted(ROOT.glob(g))]
    for binding, globs in manifest["bindings"].items():
        files = []
        for g in globs:
            hits = sorted(ROOT.glob(g))
            if not hits:
                problems.append(f"{binding}: no sources match {g}")
            files += hits
        if binding not in LINKERS:
            problems.append(f"{binding}: no link recipe in tools/check_surface.py (LINKERS)")
            continue
        names, err = LINKERS[binding](sorted({str(f) for f in files + libs}))
        if names is None:
            problems.append(f"{binding}: linking the binding failed (flags: LINKERS in tools/check_surface.py):\n"
                            + err[-1500:])
        linked[binding] = names
    rows = []
    for name in sorted(funcs):
        c = classes.get(name)
        if c is None:
            continue
        if c == "bind":
            want = list(manifest["bindings"])
        elif isinstance(c, dict) and "skip" in c and "bind" not in c:
            if not c["skip"].strip():
                problems.append(f"{name}: a skip needs a reason")
            rows.append((name, "skip", c["skip"]))
            continue
        elif isinstance(c, dict) and isinstance(c.get("bind"), list):
            want = c["bind"]
            if not c.get("skip", "").strip():
                problems.append(f"{name}: bound only for {want}: say why not the others (\"skip\")")
        else:
            problems.append(f"{name}: invalid classification {c!r}")
            continue
        missing = [b for b in want if linked.get(b) is not None and name not in linked[b]]
        for b in missing:
            problems.append(f"{b}: {name} is not reachable (classified \"bind\"; the linked binding drops it)")
        rows.append((name, "bind", ", ".join(f"{b}: {'—' if b in missing else 'ok'}" for b in want)))
    check_typings(manifest, problems)
    bound = sum(1 for r in rows if r[1] == "bind")
    print(f"{len(funcs)} public functions: {bound} bound, {len(rows) - bound} skipped with a reason")
    if "-v" in sys.argv:
        for r in rows:
            print(f"  {r[0]:34} {r[1]:5} {r[2]}")
    if problems:
        print("INCOMPLETE")
        for p in problems:
            print(" -", p)
        return 1
    print("OK: every public function is classified; every bound function survives the link of every binding;"
          " every runtime export and class member is typed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
