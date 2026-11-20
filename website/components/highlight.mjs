// Syntax highlighting for the page's code: every <code data-language> in a
// <pre>, but output and plain text, highlighted with the vendored
// highlight.js (its browser ES modules: the core and the languages below,
// mise/website.rb VENDOR).
// The colours are the design system's (style/site.css, .hljs-*). Without
// this script the code reads as plain text.
import hljs from '/vendor/highlight.js/es/core.min.js';
import bash from '/vendor/highlight.js/es/languages/bash.min.js';
import c from '/vendor/highlight.js/es/languages/c.min.js';
import javascript from '/vendor/highlight.js/es/languages/javascript.min.js';
import python from '/vendor/highlight.js/es/languages/python.min.js';
import typescript from '/vendor/highlight.js/es/languages/typescript.min.js';
import xml from '/vendor/highlight.js/es/languages/xml.min.js';

const languages = { scxml: 'xml', xml: 'xml', javascript: 'javascript', typescript: 'typescript', python: 'python', c: 'c', shell: 'bash' };
for (const [name, language] of Object.entries({ bash, c, javascript, python, typescript, xml })) hljs.registerLanguage(name, language);

for (const code of document.querySelectorAll('pre code[data-language]')) {
  const language = languages[code.dataset.language];
  if (!language) continue;
  code.innerHTML = hljs.highlight(code.textContent, { language, ignoreIllegals: true }).value;
  code.classList.add('hljs');
}
