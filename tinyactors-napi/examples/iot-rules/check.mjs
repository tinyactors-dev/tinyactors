// What the platform checks in a customer's upload before it compiles it,
// and how it fills the rule template. Nothing here runs the customer's
// code: the sandbox does that, later, with its limits.
//
// compileCharts checks the XML and the chart's structure, with line
// numbers, but not the JavaScript in it: a syntax error in an expression
// is found at its first evaluation, as an error.execution with no message
// (docs/log.xml, l-p4-examples-iot-syntax-unchecked). So the platform
// parses each expression itself, with the application's own engine
// (`new Function` parses a body and runs none of it), and reports a syntax
// error as the compiler would, `file:line:column: error: message`.

/** Escapes text for an XML attribute value or element content. */
export function escapeXML(text) {
  return String(text)
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&apos;');
}

/** Reads the five predefined entities and character references back. */
function unescapeXML(text) {
  return text.replace(/&(#x[0-9a-fA-F]+|#[0-9]+|amp|lt|gt|quot|apos);/g, (_, ref) => {
    if (ref[0] === '#') return String.fromCodePoint(ref[1] === 'x' ? parseInt(ref.slice(2), 16) : Number(ref.slice(1)));
    return { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'" }[ref];
  });
}

/** The syntax error of `source` as JavaScript, or undefined: an
 * expression when `script` is false, a script's statements when true. */
export function syntaxError(source, script = false) {
  try {
    // Parsed, never called. The newline lets a trailing `//` comment end.
    new Function(script ? source : `return (${source}\n);`);
    return undefined;
  } catch (error) {
    if (error instanceof SyntaxError) return error.message;
    throw error;
  }
}

/** Line and column (from 1) of `offset` in `text`. */
function position(text, offset) {
  const before = text.slice(0, offset).split('\n');
  return { line: before.length, column: before.at(-1).length + 1 };
}

/** The attributes of SCXML whose value is an expression. `location` is an
 * expression too (the left side of an assignment). */
const EXPRESSION_ATTRIBUTES = 'cond|expr|location|targetexpr|eventexpr|delayexpr|typeexpr|srcexpr|idlocation';

/** Every syntax error in the JavaScript of an SCXML document, as
 * diagnostics of the compiler's shape: the expressions' attributes and the
 * `<script>` bodies. */
export function checkScripts(file, text) {
  const diagnostics = [];
  const report = (offset, message) => diagnostics.push({ severity: 'error', file, ...position(text, offset), message });
  const attributes = new RegExp(`\\s(${EXPRESSION_ATTRIBUTES})\\s*=\\s*("([^"]*)"|'([^']*)')`, 'g');
  for (const match of text.matchAll(attributes)) {
    const source = unescapeXML(match[3] ?? match[4]);
    const message = syntaxError(source);
    if (message) report(match.index + 1, `${match[1]}: SyntaxError: ${message}`);
  }
  for (const match of text.matchAll(/<script(\s[^>]*)?>([\s\S]*?)<\/script>/g)) {
    const body = match[2].replace(/<!\[CDATA\[([\s\S]*?)\]\]>|([^<]+)/g, (_, data, plain) => data ?? unescapeXML(plain));
    const message = syntaxError(body, true);
    if (message) report(match.index, `<script>: SyntaxError: ${message}`);
  }
  return diagnostics;
}

/** The `name` of the document's `<scxml>` element, or undefined. */
export function chartName(text) {
  const root = /<(?:[\w.-]+:)?scxml\b([^>]*)>/.exec(text);
  return root ? /\sname\s*=\s*(?:"([^"]*)"|'([^']*)')/.exec(root[1])?.slice(1).find((v) => v !== undefined) : undefined;
}

/** The rule template with a customer's rule in it: `{{id}}`, `{{when}}`
 * and `{{message}}` replaced by the escaped text. The customer's text can
 * only ever be an attribute's value: it cannot close the attribute or add
 * markup. */
export function fillTemplate(template, { id, when, message }) {
  return template.replace(/\{\{(id|when|message)\}\}/g, (_, key) => escapeXML({ id, when, message }[key]));
}

/** A diagnostic as the compiler prints one. */
export const formatDiagnostic = ({ file, line, column, severity = 'error', message }) =>
  `${file}:${line}:${column}: ${severity}: ${message}`;
