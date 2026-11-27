// The skills this server serves, as the MCP Skills extension describes them
// (io.modelcontextprotocol/skills, Beta: modelcontextprotocol/ext-skills,
// specification/beta/skills.mdx). Read once at startup from skills/: one
// directory per skill, its SKILL.md and every other file under it.
//
// Everything that serves a skill reads this one manifest: the extension's
// methods (skills/list, skills/get, resources/directory/read), plain
// resources/list and resources/read, and the tools list_skills and
// get_skill for hosts without the extension. So a digest a host checks is
// the digest of the bytes every path serves.
//
// - A file is skill://<name>/<path>; the skill's entry is its SKILL.md,
//   skill://<name>/SKILL.md; its root skill://<name> and each subdirectory
//   are directory resources (inode/directory), no trailing slash.
// - A file's digest is sha256:<64 lowercase hex> over its raw bytes, and its
//   size their length.
// - The frontmatter is SKILL.md's YAML frontmatter as JSON, verbatim. This
//   reader takes the flat form our skills use (one `key: value` per line,
//   a value plain or quoted) and refuses anything else at startup rather
//   than serve a frontmatter that differs from the file's.

import { createHash } from 'node:crypto';
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { extname, join, relative, sep } from 'node:path';

export const EXTENSION = 'io.modelcontextprotocol/skills';
// How long a host may treat a listing or an entry as current: skills
// change only with a deploy. Not an integrity property.
export const TTL_MS = 3_600_000;
// The same for every caller: no skill depends on who asks.
export const CACHE_SCOPE = 'public';
const LIMITS = { files: 512, bytes: 16 * 1024 * 1024 };   // per skill, as the extension fixes them
const NAME = /^[a-z0-9]+(-[a-z0-9]+)*$/;                   // the Agent Skills naming rule (at most 64)

const TYPES = { '.md': 'text/markdown', '.txt': 'text/plain', '.json': 'application/json', '.py': 'text/x-python',
                '.js': 'text/javascript', '.mjs': 'text/javascript', '.sh': 'text/x-shellscript', '.scxml': 'application/scxml+xml',
                '.xml': 'application/xml', '.yaml': 'application/yaml', '.yml': 'application/yaml', '.csv': 'text/csv',
                '.png': 'image/png', '.svg': 'image/svg+xml', '.pdf': 'application/pdf' };
const TEXT = (mimeType) => mimeType.startsWith('text/') || /json|xml|yaml|javascript/.test(mimeType);
// Text only when its UTF-8 reads back to the same bytes, so a host that
// encodes the text again gets the bytes the digest covers.
const readsBack = (bytes) => Buffer.from(bytes.toString('utf8'), 'utf8').equals(bytes);

export const DIRECTORY = 'inode/directory';

// A YAML frontmatter of flat scalars, as JSON; refuses what it cannot read faithfully.
export function frontmatterOf(text, where) {
  const match = /^---\r?\n([\s\S]*?)\r?\n---(\r?\n|$)/.exec(text);
  if (!match) throw new Error(`${where}: SKILL.md does not begin with YAML frontmatter`);
  const fields = {};
  for (const line of match[1].split(/\r?\n/)) {
    if (line.trim() === '' || line.trimStart().startsWith('#')) continue;
    const pair = /^([A-Za-z0-9_-]+):[ \t]+(.*)$/.exec(line);
    if (!pair) throw new Error(`${where}: frontmatter line not a flat "key: value": ${line}`);
    let value = pair[2].trim();
    if (/^"(?:[^"\\]|\\.)*"$/.test(value)) value = JSON.parse(value);
    else if (/^'(?:[^']|'')*'$/.test(value)) value = value.slice(1, -1).replaceAll("''", "'");
    else if (/^[[{&*!|>%@`]|: | #/.test(value) || value === '') throw new Error(`${where}: frontmatter value of ${pair[1]} needs a full YAML reader: ${value}`);
    if (pair[1] in fields) throw new Error(`${where}: frontmatter repeats ${pair[1]}`);
    fields[pair[1]] = value;
  }
  return fields;
}

function filesUnder(directory) {
  const found = [];
  for (const entry of readdirSync(directory).sort()) {
    if (entry.startsWith('.')) continue;
    const path = join(directory, entry);
    if (statSync(path).isDirectory()) found.push(...filesUnder(path));
    else found.push(path);
  }
  return found;
}

// Code-point order: the same on every machine, whatever its locale.
const byCodePoint = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const digestOf = (bytes) => `sha256:${createHash('sha256').update(bytes).digest('hex')}`;

// The manifest of the skills in `directory` (none when it does not exist).
export function loadSkills(directory) {
  const skills = [];
  const files = new Map();        // uri -> {uri, skill, path, bytes, mimeType, digest, size}
  const directories = new Map();  // uri -> [child resources]
  if (!existsSync(directory)) return { skills, files, directories };
  for (const entry of readdirSync(directory).sort()) {
    const root = join(directory, entry);
    if (!statSync(root).isDirectory() || !existsSync(join(root, 'SKILL.md'))) continue;
    const frontmatter = frontmatterOf(readFileSync(join(root, 'SKILL.md'), 'utf8'), `skills/${entry}`);
    const { name, description } = frontmatter;
    if (typeof name !== 'string' || typeof description !== 'string' || !description)
      throw new Error(`skills/${entry}: SKILL.md's frontmatter needs a name and a description`);
    if (name !== entry || !NAME.test(name) || name.length > 64)
      throw new Error(`skills/${entry}: the name ${name} must be the directory's name, lowercase letters, digits and hyphens`);
    const base = `skill://${name}`;
    const own = filesUnder(root).map((path) => {
      const relativePath = relative(root, path).split(sep).join('/');
      const bytes = readFileSync(path);
      let mimeType = TYPES[extname(path).toLowerCase()] ?? 'application/octet-stream';
      if (TEXT(mimeType) && !readsBack(bytes)) mimeType = 'application/octet-stream';
      return { uri: `${base}/${relativePath}`, skill: name, path: relativePath, bytes, mimeType, digest: digestOf(bytes), size: bytes.length };
    });
    // SKILL.md first, then the others by path
    own.sort((a, b) => (a.path === 'SKILL.md' ? -1 : b.path === 'SKILL.md' ? 1 : byCodePoint(a.path, b.path)));
    const total = own.reduce((n, f) => n + f.size, 0);
    if (own.length > LIMITS.files || total > LIMITS.bytes)
      throw new Error(`skills/${entry}: ${own.length} files, ${total} bytes: past the extension's 512 files or 16 MiB a skill`);
    for (const file of own) {
      files.set(file.uri, file);
      // every directory level above the file, from the skill's root down, lists its direct child
      const parts = file.path.split('/');
      for (let depth = 0; depth < parts.length; depth++) {
        const parent = [base, ...parts.slice(0, depth)].join('/');
        const childURI = [base, ...parts.slice(0, depth + 1)].join('/');
        const isFile = depth === parts.length - 1;
        const children = directories.get(parent) ?? [];
        if (!children.some((c) => c.uri === childURI))
          children.push(isFile ? resourceOf(file, frontmatter) : { uri: childURI, name: parts[depth], mimeType: DIRECTORY });
        directories.set(parent, children);
      }
    }
    skills.push({
      uri: `${base}/SKILL.md`, frontmatter,
      resources: own.map(({ uri, digest, size }) => ({ uri, digest, size })),
    });
  }
  for (const children of directories.values()) children.sort((a, b) => byCodePoint(a.uri, b.uri));
  return { skills, files, directories };
}

// A file as a Resource: SKILL.md named and described by its frontmatter.
export function resourceOf(file, frontmatter) {
  if (file.path === 'SKILL.md')
    return { uri: file.uri, name: frontmatter.name, description: frontmatter.description, mimeType: 'text/markdown', size: file.size };
  return { uri: file.uri, name: file.path.split('/').pop(), mimeType: file.mimeType, size: file.size };
}

// A file's content as resources/read gives it: text for text, base64 for the rest.
export function contentOf(file) {
  return TEXT(file.mimeType)
    ? { uri: file.uri, mimeType: file.mimeType, text: file.bytes.toString('utf8') }
    : { uri: file.uri, mimeType: file.mimeType, blob: file.bytes.toString('base64') };
}

export const isText = (file) => TEXT(file.mimeType);
