// The inspector in Node.js: decodes sample.snapshot, a snapshot pack a
// host wrote, through the layout it describes, and prints its segments,
// pools and actors. The page decodes the same bytes in the browser with the
// same module. Run: node main.mjs [file.snapshot]
import { readFileSync, existsSync } from 'node:fs';
import { basename } from 'node:path';
import { decodePack, summarize } from './decode.mjs';

const path = process.argv[2] ?? new URL('./sample.snapshot', import.meta.url).pathname;
if (!existsSync(path)) {
  console.log(`no snapshot at ${basename(path)}`);
  process.exit(0);
}
for (const line of summarize(decodePack(readFileSync(path)), basename(path))) console.log(line);
