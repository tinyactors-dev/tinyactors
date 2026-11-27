// The issue's helpers.

function occurrence(d) {
  return { session: d.session || null, commit: d.commit || null, at: d.at || null,
           reporter: d.reporter ? d.reporter.login : (d.caller ? d.caller.login : null) };
}

// The last 50 occurrences: an issue reported a thousand times stays small.
function recent(list, item) { return list.concat([item]).slice(-50); }

function withReporter(reporters, d) {
  var id = d.caller ? d.caller.githubID : (d.reporter ? d.reporter.githubID : null);
  return id === null || reporters.indexOf(id) >= 0 ? reporters : reporters.concat([id]);
}

// A ledger record: what changed, of which issue, with what.
function record(kind, slug, n, data) {
  var clean = Object.assign({}, data);
  delete clean.replyTo;
  if (clean.caller) clean.caller = clean.caller.login;
  return { kind: kind, key: issueID(slug, n), record: clean };
}

function answer(ev, event, data) { return { to: replyTo(ev), event: event, data: data }; }

function summary(p) {
  return { n: p.n, title: p.title, excerpt: String(p.body || '').slice(0, 280), component: p.component, kind: p.kind,
           state: p.state, count: p.count, minutes: p.minutes, workaround: p.workaround ? p.workaround.text : null,
           fix: p.fix ? p.fix.sha : null, possibleDuplicateOf: p.possible || null, attachments: p.attachments || 0,
           filed: p.filed || null, updated: p.updated || null };
}

// Who may read an issue's attachments, and attach to it: its reporters and its maintainers.
function mayRead(caller, reporters, repoID) {
  return !!caller && (reporters.indexOf(caller.githubID) >= 0 || (caller.maintainerOf || []).indexOf(String(repoID)) >= 0);
}

// What a reader sees: anyone the issue, its workaround and how many files
// it has; a signed-in user also who reported it and from which sessions;
// its reporters and maintainers its attachments' descriptions (never a
// key: a download is a signed link, asked for one at a time).
function details(caller, d, reader) {
  var open = Object.assign({}, d);
  if (!caller) delete open.occurrences;
  open.attachments = reader
    ? d.attachments.map(function (a) { return { upload: a.upload, name: a.name, type: a.type, kind: a.kind, size: a.size,
                                                 by: a.by, scan: a.scan }; })
    : d.attachments.length;
  return open;
}
