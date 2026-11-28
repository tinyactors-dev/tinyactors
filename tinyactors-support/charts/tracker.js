// The tracker's helpers.

// What a pulled list changes: a new repository (with its slug), a renamed one, one archived.
function plan(repos, pulled) {
  var actions = [], taken = {};
  Object.keys(repos).forEach(function (id) { taken[repos[id].slug] = true; });
  pulled.forEach(function (repo) {
    var known = repos[repo.id];
    if (!known) {
      if (repo.archived) return;
      var slug = repo.name.split('/')[1].toLowerCase(), base = slug, k = 2;
      while (taken[slug]) slug = base + '-' + k++;   // a new repository under a name a renamed one had
      taken[slug] = true;
      actions.push({ kind: 'new', repo: repo, slug: slug });
      return;
    }
    if (known.name !== repo.name) actions.push({ kind: 'renamed', repo: repo });
    if (repo.archived && !known.archived) actions.push({ kind: 'archived', repo: repo });
  });
  return actions;
}

// The inventory after a pull: every repository listed, archived ones kept
// (their issues stay readable), ones never spawned left out.
function inventory(repos, pulled, actions) {
  var next = Object.assign({}, repos), slugs = {};
  actions.forEach(function (a) { if (a.kind === 'new') slugs[a.repo.id] = a.slug; });
  pulled.forEach(function (repo) {
    var known = repos[repo.id];
    if (!known && repo.archived) return;
    var names = known ? known.names : [];
    next[repo.id] = { name: repo.name, slug: known ? known.slug : slugs[repo.id], archived: !!repo.archived,
                      names: names.indexOf(repo.name) >= 0 ? names : names.concat([repo.name]) };
  });
  return next;
}

// Every name a repository had, and its slug, reaches it.
function names(repos) {
  var byName = {};
  Object.keys(repos).forEach(function (id) {
    repos[id].names.forEach(function (name) { byName[name] = id; });
    byName[repos[id].slug] = id;
  });
  return byName;
}

function validNames(repos) {
  return Object.keys(repos).filter(function (id) { return !repos[id].archived; })
    .map(function (id) { return repos[id].name; }).sort();
}

function projectList(repos) {
  return Object.keys(repos).map(function (id) {
    return { repoID: id, name: repos[id].name, slug: repos[id].slug, archived: repos[id].archived, names: repos[id].names };
  });
}

// Why a request cannot be routed, or null. Reads of an archived
// repository's issues are routed; the project refuses new reports.
function refusal(repos, byName, ev) {
  var data = ev.data || {};
  if (typeof data.repo !== 'string') return 'no repository named';
  if (!has(byName, data.repo)) return 'unknown repository ' + data.repo;
  return null;
}

function waitFor(creating, sid, ev) {
  var entry = creating[sid];
  return put(creating, sid, { repoID: entry.repoID, waiting: push(entry.waiting, { event: ev.name, data: forwarded(ev) }) });
}
