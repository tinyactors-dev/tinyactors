// The issue's helpers.

// A report as the catalog records it (its time is the write's).
function occurrence(d) {
  var who = d.caller || d.reporter || null;
  return { session: d.session || null, commit: d.commit || null, minutes: d.minutes || 0,
           reporter: who ? who.login : null, reporterID: who ? who.githubID : null };
}

function withReporter(reporters, d) {
  var id = d.caller ? d.caller.githubID : (d.reporter ? d.reporter.githubID : null);
  return id === null || reporters.indexOf(id) >= 0 ? reporters : reporters.concat([id]);
}

// A change, for the ledger and the catalog: what changed, of which issue
// (and its repository), with what.
function record(kind, repoID, slug, n, data) {
  var clean = Object.assign({}, data, { repoID: repoID });
  delete clean.replyTo;
  if (clean.caller) clean.caller = clean.caller.login;
  return { kind: kind, key: issueID(slug, n), record: clean };
}

function answer(ev, event, data) { return { to: replyTo(ev), event: event, data: data }; }

// Who may read an issue's attachments, and attach to it: its reporters and its maintainers.
function mayRead(caller, reporters, repoID) {
  return !!caller && (reporters.indexOf(caller.githubID) >= 0 || (caller.maintainerOf || []).indexOf(String(repoID)) >= 0);
}
