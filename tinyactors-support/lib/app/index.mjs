// The issue list, an MCP App (io.modelcontextprotocol/ui): one HTML file,
// built by tools/build-app.mjs from lib/app/source/ and the design
// system's tokens, served as the resource ui://tinyactors-support/issues.
import { readFileSync } from 'node:fs';

export const APP = {
  mimeType: 'text/html;profile=mcp-app',
  html: readFileSync(new URL('./issues.html', import.meta.url), 'utf8'),
  // it loads nothing from anywhere (an empty CSP: the host's restrictive
  // default), and asks for the host's border around it
  meta: { ui: { prefersBorder: true, csp: {} } },
};
