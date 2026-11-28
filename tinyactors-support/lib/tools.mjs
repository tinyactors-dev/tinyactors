// The MCP tools: each one's input schema, output schema and the line of
// text that goes with its result.
//
// The rules (the maintainer, 2026-11-27):
// - JSON goes only into structured output (`structuredContent`), never into
//   the text content.
// - Every tool has an output schema, so a model writing code against the
//   tools (code mode) gets typed results. Every answer conforms, refusals
//   and errors included (test/tools.test.mjs).
// - The text is short and says only what matters: what happened, the
//   identifier, what to do next.
// - Data that reads better as a view than as text gets an MCP App: the
//   issues (known_issues, get_issue) open the issue list (lib/app/).

export const APP_URI = 'ui://tinyactors-support/issues';

const text = (description) => ({ type: 'string', description });
const number = (description) => ({ type: 'number', description });
const boolean = (description) => ({ type: 'boolean', description });
const object = (properties, required = []) => ({ type: 'object', properties, required, additionalProperties: false });
const array = (items, description) => ({ type: 'array', items, description });
const nullable = (schema) => ({ ...schema, type: [schema.type, 'null'].flat() });
const issueID = text('the issue, tinyactors:<project>:<n>');
const KINDS = ['docs', 'error', 'tooling', 'api', 'environment'];
const STATES = ['new', 'triaged', 'regressed', 'fixed', 'closed', 'wontfix'];

// --- output schemas ---------------------------------------------------------

// What every answer may carry: an error (an unknown tool, an identifier
// that is not one) and, for a signed-in caller, the news since their last
// call: their issues that were fixed, regressed or got a workaround or a file.
const news = object({
  id: issueID, repo: text('its repository, owner/name'), n: number('its number in the project'),
  title: text('its title'), state: text('what happened: fixed, regressed, workaround, attachment, attachment rejected'),
  sha: text('the commit, for fixed and regressed'), text: text('the workaround'),
  file: text('the attachment\'s name'), by: text('who attached it'), reason: text('why an attachment was rejected'),
});
const common = {
  error: text('set when the call failed before reaching the tracker: an unknown tool, an identifier that is not one'),
  sinceYourLastVisit: array(news, 'what changed for you since your last call (signed in only)'),
};
const output = (properties, answers) => ({
  type: 'object',
  properties: { ...(answers ? { answer: { type: 'string', enum: answers, description: 'what happened' } } : {}),
                ...properties, ...common },
  additionalProperties: false,
});
const reference = object({ id: issueID, repo: text('owner/name'), n: number('its number'), title: text('its title'),
                           p: number('the decision model\'s probability that it is the same problem') });
const workaround = object({ text: text('the workaround'), by: text('who wrote it') });
const scan = object({ verdict: text('ok, redacted or refused'), redactions: number('secrets taken out of a log'),
                      summary: nullable(text('what the check saw: an archive\'s entries, a snapshot\'s summary')) });
const summary = object({
  id: issueID, repo: text('owner/name'), n: number('its number'), title: text('its title'),
  excerpt: text('the start of its first report'), component: text('its top-level directory'),
  kind: { type: 'string', enum: KINDS }, state: { type: 'string', enum: STATES },
  count: number('how many reports'), minutes: number('the minutes it cost, all reports together'),
  workaround: nullable(text('the workaround')), fix: nullable(text('the fixing commit')),
  possibleDuplicateOf: nullable(reference), attachments: number('how many files'),
  filed: nullable(text('when it was filed (ISO 8601); null for an issue older than these fields, until its next change')),
  updated: nullable(text('when it last changed (ISO 8601); null as filed')),
});
const refusal = { reason: text('why it was refused'), valid: array({ type: 'string' }, 'what would be accepted') };

const OUTPUT = {
  list_skills: output({ skills: array(object({ name: text('the skill'), description: text('what it is for'),
                                               uri: text('its SKILL.md as a resource, skill://<name>/SKILL.md (the Skills extension\'s skills/get takes it)') }),
                                       'the skills') }),
  get_skill: output({ name: text('the skill'), description: text('what it is for'),
                      path: text('the file: SKILL.md, or one of files'), text: text('the file, Markdown'),
                      uri: text('the file as a resource, skill://<name>/<path>'),
                      digest: text('sha256:<hex> of the file\'s bytes, as the Skills extension\'s manifest gives it'),
                      size: number('the file\'s length in bytes'),
                      files: array({ type: 'string' }, 'its other files, by path, for get_skill with path'),
                      ...refusal }, ['refused']),
  search_docs: output({ hits: array(object({ path: text('its path, for get_doc'), title: text('its title'),
                                             score: number('how well it matched'), excerpt: text('where the words are') }),
                                    'the documents, best first') }),
  get_doc: output({ path: text('its path'), title: text('its title'), text: text('the whole document, Markdown'),
                    ...refusal }, ['refused']),
  list_projects: output({ projects: array(object({ repoID: text('its GitHub id, as whoami\'s maintainerOf lists it'),
                                                   repo: text('owner/name'), slug: text('the <project> of its identifiers'),
                                                   archived: boolean('archived on GitHub: no new reports'),
                                                   components: array({ type: 'string' }, 'its top-level directories') }),
                                          'every repository the tracker knows') }),
  known_issues: output({ issues: array(summary, 'the issues, costliest first (reports times minutes), or best match first for a query') }),
  get_issue: output({
    id: issueID, repo: text('owner/name'), n: number('its number'), title: text('its title'),
    state: { type: 'string', enum: STATES }, component: text('its top-level directory'), kind: { type: 'string', enum: KINDS },
    body: text('its first report'), count: number('how many reports'), minutes: number('the minutes it cost'),
    workaround: nullable(workaround),
    fix: nullable(object({ sha: text('the commit'), url: text('the commit on GitHub'), repoID: number('the GitHub id of the repository the commit is in') })),
    filed: nullable(text('when it was filed (ISO 8601); null for an issue older than this field, until its next change')),
    updated: nullable(text('when it last changed (ISO 8601)')),
    occurrences: array(object({ session: nullable(text('the reporter\'s session')), commit: nullable(text('their commit')),
                                at: nullable(text('when (ISO 8601)')), reporter: nullable(text('their login')) }), 'the last 50 reports (signed in only)'),
    attachments: { anyOf: [number('how many files (for anyone but its reporters and maintainers)'),
                            array(object({ upload: text('for get_attachment'), name: text('the file'), type: text('its media type'),
                                           kind: text('screenshot, recording, log, archive or tinyactors'), size: number('bytes'),
                                           by: text('who attached it'), scan }), 'its files')] },
    valid: refusal.valid,
    reason: nullable(text('why it was declined, or why the read was refused')),
  }, ['issue', 'refused', 'failed']),
  whoami: output({ login: text('your GitHub login'), maintainerOf: array({ type: 'string' }, 'the repositories you maintain (GitHub ids)'),
                   quota: object({ files: number('attachments left today'), bytes: number('bytes left today') }) }),
  report_issue: output({
    id: issueID, repo: text('owner/name'), n: number('its number'),
    similar: array(reference, 'open issues of the component that share words with it: did you mean'),
    possibleDuplicateOf: nullable(reference),
    count: number('how many reports it has now'), workaround: nullable(workaround),
    matchedBy: nullable(object({ model: text('jev'), confidence: number('how sure') })),
    state: text('closed or wontfix: what it is now'), reason: text('why it was declined, or refused'),
    fix: { anyOf: [nullable(object({ sha: text('the commit'), url: text('on GitHub'), repoID: number('its repository\'s GitHub id') })),
                   text('the fixing commit (regressed)')] },
    sha: text('the fixing commit (fixed)'), checked: boolean('whether your commit was compared with the fix'),
    advice: text('what to do: update your checkout'), commit: text('your commit, which has the fix (regressed)'),
    valid: refusal.valid,
  }, ['filed', 'counted', 'fixed', 'regressed', 'refused', 'failed']),
  attach_workaround: output({ id: issueID, repo: text('owner/name'), ...refusal }, ['noted', 'refused', 'failed']),
  attach_file: output({
    uploadID: text('the slot'), method: text('PUT'), maxBytes: number('the size you declared'),
    expiresIn: number('seconds the slot stays open'), uploadURL: text('where to PUT the bytes'), how: text('the curl command'),
    ...refusal,
  }, ['upload.opened', 'refused', 'failed']),
  get_attachment: output({
    upload: text('the attachment'), name: text('the file'), type: text('its media type'), kind: text('its kind'),
    size: number('bytes'), sha256: text('its SHA-256'), key: text('where it is stored'), by: text('who attached it'),
    byID: number('their GitHub id'), scan, url: text('a signed download link'), expiresIn: number('seconds the link works'),
    ...refusal,
  }, ['attachment', 'refused', 'failed']),
  triage: output({ id: issueID, repo: text('owner/name'), ...refusal }, ['triaged', 'refused', 'failed']),
  close: output({ id: issueID, repo: text('owner/name'), ...refusal }, ['closed', 'refused', 'failed']),
  wontfix: output({ id: issueID, repo: text('owner/name'), ...refusal }, ['wontfix', 'refused', 'failed']),
  delete_issue: output({ id: issueID, repo: text('owner/name'), ...refusal }, ['deleted', 'refused', 'failed']),
};

// --- the tools ------------------------------------------------------------------

const app = { ui: { resourceUri: APP_URI } };

export const TOOLS = {
  // anyone
  list_skills: { access: 'anyone', description: 'The skills for working with tinyactors (each also a resource)',
    input: object({}) },
  get_skill: { access: 'anyone', description: "A skill's SKILL.md, or with path one of the files it names (references/...)",
    input: object({ name: text('the skill'), path: text('a file of the skill, as files lists it; SKILL.md when absent') }, ['name']) },
  search_docs: { access: 'anyone', description: 'Search the documentation of tinyactors',
    input: object({ query: text('words to look for') }, ['query']) },
  get_doc: { access: 'anyone', description: 'A document search_docs found, whole', input: object({ path: text('its path, as search_docs gives it') }, ['path']) },
  list_projects: { access: 'anyone', description: 'The repositories issues are filed against, with their components',
    input: object({}) },
  known_issues: { access: 'anyone', meta: app,
    description: 'The open issues, costliest first, with their workarounds: look here before you report. Shows the issue list where the host renders apps.',
    input: object({ repo: text('owner/name, or a project\'s slug; all when absent'), component: text('a top-level directory'),
      state: { type: 'string', enum: ['open', 'all', ...STATES], description: 'open (new, triaged, regressed; the default), all, or one state' },
      query: text('words to search titles, reports and workarounds for (full text, stemmed); the best matches first') }) },
  get_issue: { access: 'anyone', meta: app, description: 'One issue: its state, workaround, fix and attachments',
    input: object({ id: issueID }, ['id']) },
  // signed in
  whoami: { access: 'user', description: 'Who you are, and what changed for you since your last call', input: object({}) },
  report_issue: { access: 'user',
    description: 'Report what got in your way with tinyactors. The answer says whether it is new, known (with its workaround), fixed (update), or regressed.',
    input: object({
      repo: text('the repository, owner/name (tinyactors-dev/tinyactors for the monorepo)'),
      component: text('its top-level directory (tinyactors-napi, docs, ...); a refusal lists the valid ones'),
      title: text('what went wrong, in a line'),
      body: text('what you did, what happened, what you expected; the relevant lines of output'),
      kind: { type: 'string', enum: KINDS },
      minutes: number('the minutes it cost you'),
      commit: text('the commit you were on (git rev-parse HEAD)'),
      session: text('a link to your session, if you have one'),
      id: text('an issue it is an occurrence of, tinyactors:<project>:<n>'),
    }, ['repo', 'component', 'title']) },
  attach_workaround: { access: 'user', description: 'Tell the next agent how you got past an issue',
    input: object({ id: issueID, text: text('the workaround') }, ['id', 'text']) },
  attach_file: { access: 'user',
    description: 'Open a slot for a file (a log, screenshot, recording, zip, snapshot), then PUT its bytes there with curl: a tool call cannot carry them',
    input: object({ id: issueID, name: text('the file name'), type: text('its media type (text/plain, image/png, application/zip, ...)'),
      size: number('its size in bytes'), sha256: text('its SHA-256, hexadecimal') }, ['id', 'name', 'type', 'size', 'sha256']) },
  get_attachment: { access: 'user', description: "A signed link to one of an issue's attachments",
    input: object({ id: issueID, upload: text('the attachment, as get_issue lists it') }, ['id', 'upload']) },
  // a maintainer of the repository
  triage: { access: 'maintainer', description: 'Accept a new issue, set its kind or title',
    input: object({ id: issueID, kind: { type: 'string', enum: KINDS }, title: text('a better title') }, ['id']) },
  close: { access: 'maintainer', description: 'Close a fixed issue', input: object({ id: issueID }, ['id']) },
  wontfix: { access: 'maintainer', description: 'Decline an open issue, with the reason agents will see',
    input: object({ id: issueID, reason: text('why') }, ['id', 'reason']) },
  delete_issue: { access: 'maintainer',
    description: 'Delete an issue outright (spam, a test report): it leaves every list, its files are deleted, its number is not reused, the ledger records who and why',
    input: object({ id: issueID, reason: text('why, for the ledger') }, ['id']) },
};
for (const [name, tool] of Object.entries(TOOLS)) tool.output = OUTPUT[name];

// --- the text ---------------------------------------------------------------------

const plural = (count, one, many = `${one}s`) => `${count} ${count === 1 ? one : many}`;
const LIST = 10;   // lines of a list in the text; the rest is in the structured result

function newsLine(items) {
  if (!items?.length) return '';
  const each = items.slice(0, 5).map((u) => `${u.id} ${u.state}${u.sha ? ` in ${u.sha}` : ''}${u.file ? ` (${u.file})` : ''}`);
  return `\nSince your last call: ${each.join('; ')}${items.length > 5 ? `; ${items.length - 5} more` : ''}.`;
}

function refused(r) {
  return `Refused: ${r.reason ?? 'no reason given'}${r.valid?.length ? ` (valid: ${r.valid.join(', ')})` : ''}.`;
}

// The line (or two, or a short list) of text for a result. Never JSON.
export function summarize(name, r) {
  if (r.error) return `Error: ${r.error}.`;
  if (r.answer === 'refused') return refused(r) + newsLine(r.sinceYourLastVisit);
  if (r.answer === 'failed' || r.answer === 'timeout') return `Failed: ${r.reason ?? 'the tracker did not answer'}. Try again.`;
  const line = (() => {
    switch (name) {
      case 'list_skills': return r.skills.length ? `${plural(r.skills.length, 'skill')}: ${r.skills.map((k) => k.name).join(', ')}.` : 'No skills.';
      case 'get_skill': return r.text;
      case 'get_doc': return r.text;
      case 'search_docs': return r.hits.length
        ? r.hits.slice(0, LIST).map((h) => `${h.path}: ${h.title}`).join('\n') + (r.hits.length > LIST ? `\n${r.hits.length - LIST} more.` : '')
        : 'No document matches.';
      case 'list_projects': return r.projects.map((p) => `${p.repo} (${p.slug}${p.archived ? ', archived' : ''})`).join('\n') || 'No projects.';
      case 'known_issues': return r.issues.length
        ? r.issues.slice(0, LIST).map((i) => `${i.id} [${i.state}] ${i.title} (${plural(i.count, 'report')}, ${i.minutes} min)` +
            `${i.workaround ? `; workaround: ${i.workaround}` : ''}${i.fix ? `; fixed in ${i.fix}` : ''}`).join('\n') +
          (r.issues.length > LIST ? `\n${r.issues.length - LIST} more.` : '')
        : 'No such issues.';
      case 'get_issue': return `${r.id} [${r.state}] ${r.title} (${plural(r.count, 'report')}, ${r.minutes} min)` +
        `${r.workaround ? `\nWorkaround: ${r.workaround.text}` : ''}${r.fix ? `\nFixed in ${r.fix.sha}.` : ''}${r.reason ? `\nDeclined: ${r.reason}` : ''}`;
      case 'whoami': return `${r.login}; maintainer of ${plural(r.maintainerOf.length, 'repository', 'repositories')}; ` +
        `${plural(r.quota.files, 'file')} left to attach today.`;
      case 'report_issue':
        switch (r.answer) {
          case 'filed': return `Filed ${r.id}.` +
            (r.possibleDuplicateOf ? ` It may be ${r.possibleDuplicateOf.id} (${r.possibleDuplicateOf.title ?? 'a maintainer decides'}).` : '') +
            (r.similar?.length ? ` Similar: ${r.similar.map((s) => s.id).join(', ')}.` : '');
          case 'counted': return `Counted on ${r.id} (${plural(r.count, 'report')})` +
            (r.state ? `, which is ${r.state}${r.reason ? `: ${r.reason}` : ''}` : '') +
            (r.workaround ? `; workaround: ${r.workaround.text}` : '') + '.';
          case 'fixed': return `${r.id} is fixed in ${r.sha}${r.advice ? `: ${r.advice}` : '; update your checkout'}.`;
          case 'regressed': return `${r.id} came back: your commit ${r.commit} has its fix ${r.fix}. Reopened.`;
        }
        return `${r.answer} ${r.id ?? ''}`.trim();
      case 'attach_workaround': return `Workaround noted on ${r.id}.`;
      case 'attach_file': return `Upload slot open for ${Math.round(r.expiresIn / 60)} minutes:\n${r.how}`;
      case 'get_attachment': return `${r.name} (${r.size} bytes, ${r.scan.verdict}), link valid ${Math.round(r.expiresIn / 60)} minutes:\n${r.url}`;
      case 'triage': return `Triaged ${r.id}.`;
      case 'close': return `Closed ${r.id}.`;
      case 'wontfix': return `Declined ${r.id}.`;
      case 'delete_issue': return `Deleted ${r.id}.`;
    }
    return 'Done.';
  })();
  return line + newsLine(r.sinceYourLastVisit);
}
