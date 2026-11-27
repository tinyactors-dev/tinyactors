// Helpers every chart shares: plain functions of their arguments. The
// compile puts this file in front of each chart's own helper file (a
// chart with none reads it alone): on the native datamodel a helper sees
// the helpers of its own script only.
// Names follow one convention: the kind and the ids joined by "."
// (tracker, project.<repoID>, issue.<repoID>.<n>, auth, login.<state>,
// user.<githubID>, grant.<id>), so an address is the name after "@".

function addr(name) { return 'tinyactors://support/@' + name; }

// Whom to answer: the original asker when a request was forwarded, else the sender.
function replyTo(ev) { return (ev.data && ev.data.replyTo) || ev.origin; }

// The request's data, with whom to answer, for forwarding it.
function forwarded(ev) { return Object.assign({}, ev.data, { replyTo: replyTo(ev) }); }

// An issue's identifier: tinyactors:<project>:<n>, where <project> is
// the project's slug, fixed when the repository was first seen (its name
// then), so an identifier never changes, even when the repository is renamed.
function issueID(slug, n) { return 'tinyactors:' + slug + ':' + n; }
function parseIssueID(text) {
  var m = /^tinyactors:([a-z0-9._-]+):([0-9]+)$/.exec(String(text));
  return m ? { slug: m[1], n: Number(m[2]) } : null;
}

// The issues a commit fixes: its git trailers "Fixes-issue: <identifier>",
// one per line, any project's.
function fixedIssues(message) {
  var re = /^Fixes-issue:[ \t]*(\S+)[ \t]*$/gim, m, found = [];
  while ((m = re.exec(message)) !== null) { var id = parseIssueID(m[1]); if (id) found.push(id); }
  return found;
}

function put(map, key, value) { var m = Object.assign({}, map); m[key] = value; return m; }
function drop(map, key) { var m = Object.assign({}, map); delete m[key]; return m; }
function push(list, value) { return list.concat([value]); }
function has(map, key) { return Object.prototype.hasOwnProperty.call(map, key); }

// Words of a title, for matching and for "did you mean".
function words(text) {
  return String(text).toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').split(' ')
    .filter(function (w) { return w.length > 2; });
}
function fingerprint(component, title) {
  return component + ':' + words(title).filter(function (w, i, all) { return all.indexOf(w) === i; }).sort().join(' ');
}
