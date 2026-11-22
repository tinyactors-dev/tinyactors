# Platform icons

The icons the pages show for a package's platforms (`runs-on` in
`content/site.xml`; `render/site.xsl`, `f:platform`). The build copies
them to `/assets/platforms/`.

| File | Source | Licence |
|---|---|---|
| `node.svg` | Devicon 2.16.0, `icons/nodejs/nodejs-original.svg` | MIT, Copyright (c) 2015 konpa |
| `bun.svg` | Devicon 2.16.0, `icons/bun/bun-original.svg` | MIT, Copyright (c) 2015 konpa |
| `python.svg` | Devicon 2.16.0, `icons/python/python-original.svg` | MIT, Copyright (c) 2015 konpa |
| `c.svg` | Devicon 2.16.0, `icons/c/c-original.svg` (the hexagonal tile; 2.18.0 replaced it with a grey letter) | MIT, Copyright (c) 2015 konpa |
| `browser.svg` | lucide-static 0.460.0, `icons/globe.svg` (the design system's icons) | ISC |

Copied rather than installed: Devicon's package is 59 MB for these four
files. The logos are their projects' marks, used to name the platform.

The four logos are served as image files, each its own document: Node.js's
and Python's gradients have ids, which would repeat in a page that inlined
an icon twice. The globe has one colour and is inlined, so it takes the
text's colour and follows the theme.
