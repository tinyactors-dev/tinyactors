"""Serial, review-gated implementation driver. The verifier owns check verdicts."""
import fnmatch
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import subprocess
import sys
import time
import uuid

from .catalog import BASE, CLASSES, build, digest, owner
from .runner import capture, child_env, environment, select as select_checks
from .store import Store, now

# Some independent probes require later classes. They remain mandatory at integration.
DEFERRED = ('behavior.runtime.*', 'behavior.high_reply.*')
ORDER = [BASE + '00-bootstrap.md', owner('raw.Result'), owner('raw.Value'), owner('raw.Memory'),
         BASE + '01-records.md', BASE + '02-callbacks.md']
ORDER += [owner('raw.' + name) for name in
          ('Allocator', 'Arena', 'System', 'Definition', 'ActorRef', 'Context', 'RequestRef', 'InvocationRef', 'InspectCursor')]
ORDER += [owner('high.' + name) for name in
          ('Error', 'Event', 'Target', 'Message', 'CodeRegistry', 'Action', 'Send', 'Invoke',
           'StateBuilder', 'MachineBuilder', 'Definition', 'Context', 'DataModel', 'Snapshot',
           'Inspection', 'Actor', 'Invocation', 'IORequest', 'System')]
ORDER += [BASE + '99-integration.md']
SHARED_FILES = ['bindings/ruby/lib/tinyactors.rb', 'bindings/ruby/lib/tinyactors/raw.rb',
                'bindings/ruby/ext/tinyactors_raw/extconf.rb',
                'bindings/ruby/ext/tinyactors_raw/tinyactors_raw.c',
                'bindings/ruby/ext/tinyactors_raw/support.h', 'bindings/ruby/ext/tinyactors_raw/support.c',
                'bindings/ruby/ext/tinyactors_raw/registrations.h',
                'bindings/ruby/test/test_helper.rb', 'bindings/ruby/test/support/*']
IMPLEMENT_SCHEMA = {'type': 'object', 'additionalProperties': False,
                    'properties': {'summary': {'type': 'string'}, 'blockers': {'type': 'array', 'items': {'type': 'string'}}},
                    'required': ['summary', 'blockers']}
REVIEW_SCHEMA = {'type': 'object', 'additionalProperties': False, 'properties': {
    'verdict': {'type': 'string', 'enum': ['approve', 'changes_requested']}, 'summary': {'type': 'string'},
    'findings': {'type': 'array', 'items': {'type': 'object', 'additionalProperties': False,
        'properties': {key: {'type': 'string'} for key in ('priority', 'path', 'detail')},
        'required': ['priority', 'path', 'detail']}}}, 'required': ['verdict', 'summary', 'findings']}


class DriverError(RuntimeError):
    pass


def git(root, *args, input=None):
    try:
        p = subprocess.run(['git', *args], cwd=root, input=input, capture_output=True, timeout=120)
    except subprocess.TimeoutExpired as error:
        raise DriverError('Git command timed out: ' + ' '.join(args[:2])) from error
    if p.returncode:
        raise DriverError(p.stderr.decode(errors='replace').strip())
    return p.stdout.decode(errors='replace').strip()


def changes(root):
    # Include staged, unstaged, and untracked source changes. NUL protects unusual filenames.
    tracked = subprocess.check_output(['git', 'diff', '--name-only', '-z', 'HEAD'], cwd=root).split(b'\0')
    untracked = subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=root).split(b'\0')
    return sorted({os.fsdecode(p) for p in tracked + untracked if p})


def task_ids(checks, brief):
    if brief == BASE + '99-integration.md':
        return {c['id'] for c in checks}
    return {c['id'] for c in checks if c.get('shared') or
            (c['brief'] == brief and not any(fnmatch.fnmatchcase(c['id'], p) for p in DEFERRED))}


def next_task(checks, rows):
    passed = {r['check_id'] for r in rows if r['status'] == 'pass'}
    for brief in ORDER:
        if not task_ids(checks, brief) <= passed:
            return brief
    return None


def owned_files(root, brief):
    import re
    infrastructure = {
        BASE + '01-records.md': ['ext/tinyactors_raw/records.c', 'ext/tinyactors_raw/records.h',
                                 'test/raw/records_test.rb', 'briefs/raw-records.md'],
        BASE + '02-callbacks.md': ['ext/tinyactors_raw/callbacks.c', 'ext/tinyactors_raw/callbacks.h', 'test/raw/callbacks_test.rb']}
    if brief in infrastructure:
        files = ['bindings/ruby/' + name for name in infrastructure[brief]]
    else:
        sections = (root / brief).read_text().split('## Owned files', 1)
        if len(sections) != 2: raise DriverError('Assignment has no owned-file declaration: ' + brief)
        body = sections[1].split('\n## ', 1)[0]
        files = ['bindings/ruby/' + value for value in re.findall(r'`([^`]+)`', body) if '/' in value or value == 'README.md']
    files += SHARED_FILES
    # Context and infrastructure tests can use private public-C-API fixtures.
    if brief == BASE + '99-integration.md':
        files += ['bindings/ruby/lib/*', 'bindings/ruby/ext/*', 'bindings/ruby/test/*', 'bindings/ruby/examples/*']
    files += ['bindings/ruby/api-coverage.json']
    return files


def validate_review(report):
    if not isinstance(report, dict) or set(report) != {'verdict', 'summary', 'findings'}:
        raise DriverError('Reviewer returned an invalid report')
    if report['verdict'] not in ('approve', 'changes_requested') or not isinstance(report['summary'], str):
        raise DriverError('Reviewer returned an invalid verdict')
    if not isinstance(report['findings'], list):
        raise DriverError('Reviewer findings must be a list')
    for finding in report['findings']:
        if not isinstance(finding, dict) or set(finding) != {'priority', 'path', 'detail'} or not all(isinstance(v, str) for v in finding.values()):
            raise DriverError('Reviewer finding is malformed')
    if report['verdict'] == 'approve' and report['findings']:
        raise DriverError('An approval cannot contain unresolved findings')
    if report['verdict'] == 'changes_requested' and not report['findings']:
        raise DriverError('A rejection must identify a concrete finding')
    return report


def acceptance(before, after, selected):
    previous_passes = {r['check_id'] for r in before if r['status'] == 'pass'}
    passes = {r['check_id'] for r in after if r['status'] == 'pass'}
    missing = sorted(selected - passes)
    regressions = sorted(previous_passes - passes)
    return missing, regressions


def stream(argv, cwd, prefix, emit, timeout, prompt=None, codex=False):
    """Capture raw logs, summarize JSON events, and keep a heartbeat while commands run."""
    prefix.parent.mkdir(parents=True, exist_ok=True)
    started = heartbeat = time.monotonic()
    summary = {'thread_id': None, 'turn_completed': False, 'failed': False, 'usage': {}, 'last_json': None}
    with Path(str(prefix) + '.stdout').open('wb') as out, Path(str(prefix) + '.stderr').open('wb') as err:
        input_file = None
        if prompt is not None:
            input_path = Path(str(prefix) + '.stdin')
            input_path.write_text(prompt)
            input_file = input_path.open('rb')
        child = subprocess.Popen(argv, cwd=cwd, stdin=input_file if input_file else subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=err, start_new_session=True, env=child_env())
        if input_file: input_file.close()
        selector = selectors.DefaultSelector()
        selector.register(child.stdout, selectors.EVENT_READ)
        pending = b''
        total = 0
        try:
            while selector.get_map():
                elapsed = time.monotonic() - started
                if elapsed > timeout:
                    raise DriverError(f'Command timed out after {timeout}s; logs: {prefix}')
                if time.monotonic() - heartbeat >= 30:
                    emit(f'Waiting for {"agent" if codex else "verification"}: {int(elapsed)}s elapsed')
                    heartbeat = time.monotonic()
                for key, _ in selector.select(.25):
                    data = os.read(key.fd, 65536)
                    if not data:
                        selector.unregister(key.fileobj)
                        continue
                    out.write(data)
                    out.flush()
                    total += len(data)
                    if total + os.fstat(err.fileno()).st_size > 50 * 1024 * 1024:
                        raise DriverError('Command exceeded the 50 MiB log limit')
                    pending += data
                    while b'\n' in pending:
                        line, pending = pending.split(b'\n', 1)
                        text = line.decode(errors='replace')
                        try:
                            event = json.loads(text)
                        except ValueError:
                            if not codex and text.strip(): emit(text[:500])
                            continue
                        if not isinstance(event, dict): continue
                        summary['last_json'] = event
                        if not codex: continue
                        kind = event.get('type')
                        if kind == 'thread.started': summary['thread_id'] = event.get('thread_id')
                        if kind in ('turn.failed', 'error'): summary['failed'] = True
                        if kind == 'turn.completed':
                            summary['turn_completed'] = True
                            summary['usage'] = event.get('usage', {})
                        item = event.get('item', {})
                        if kind == 'item.completed' and item.get('type') == 'agent_message':
                            emit('Agent: ' + item.get('text', '').replace('\n', ' ')[:500])
            # Closing stdout is not proof that a process has exited.
            child.wait(timeout=max(.1, timeout - (time.monotonic() - started)))
        except subprocess.TimeoutExpired as error:
            raise DriverError('Command closed stdout but did not exit') from error
        finally:
            selector.close()
            try: os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            child.wait()
            child.stdout.close()
        summary['exit_code'] = child.returncode
    if codex and (summary['exit_code'] != 0 or summary['failed'] or not summary['turn_completed']):
        raise DriverError(f'Agent did not complete successfully; logs: {prefix}')
    return summary


class Driver:
    def __init__(self, root, store, options):
        self.root, self.store, self.options = root.resolve(), store, options
        self.state = None
        self.store.db.executescript('''
          CREATE TABLE IF NOT EXISTS driver_loops(loop_id TEXT PRIMARY KEY, state_json TEXT NOT NULL, updated_at TEXT NOT NULL);
          CREATE TABLE IF NOT EXISTS driver_iterations(loop_id TEXT NOT NULL, number INTEGER NOT NULL,
            record_json TEXT NOT NULL, updated_at TEXT NOT NULL, PRIMARY KEY(loop_id,number));
        ''')

    def save(self):
        self.store.db.execute('INSERT INTO driver_loops VALUES(?,?,?) ON CONFLICT(loop_id) DO UPDATE SET state_json=excluded.state_json,updated_at=excluded.updated_at',
                              (self.state['loop_id'], json.dumps(self.state), now()))

    def iteration_record(self, record):
        self.store.db.execute('INSERT INTO driver_iterations VALUES(?,?,?,?) ON CONFLICT(loop_id,number) DO UPDATE SET record_json=excluded.record_json,updated_at=excluded.updated_at',
                              (self.state['loop_id'], record['number'], json.dumps(record), now()))

    def emit(self, text):
        line = f'[{now()}] {text}'
        print(line, flush=True)
        if self.state:
            with (Path(self.state['directory']) / 'progress.log').open('a') as log: log.write(line + '\n')

    @property
    def worktree(self): return Path(self.state['worktree'])

    def initialize(self, resume=None, overrides=None):
        if resume:
            row = self.store.db.execute('SELECT state_json FROM driver_loops WHERE loop_id=?', (resume,)).fetchone()
            if not row: raise DriverError('Unknown loop ID: ' + resume)
            self.state = json.loads(row[0])
            if self.state['root'] != str(self.root): raise DriverError('Resume from the original driver checkout')
            if not self.worktree.is_dir(): raise DriverError('The retained worktree is missing')
            if git(self.worktree, 'rev-parse', 'HEAD') != self.state['accepted_head']:
                raise DriverError('Worktree HEAD changed outside the driver; inspect it before resuming')
            self.options = self.state['options']
            self.options.update(overrides or {})
            self.assert_trusted()
            self.state['status'] = 'running'
            self.state['stalls'] = 0
            self.save()
            self.emit('Resumed loop ' + resume)
            return
        checks = build(self.root)
        if {c['brief'] for c in checks} - set(ORDER): raise DriverError('A catalog brief has no driver assignment')
        if len(ORDER) != len(set(ORDER)): raise DriverError('Duplicate driver assignments')
        id = uuid.uuid4().hex[:12]
        directory = self.store.path.parent / 'driver' / id
        directory.mkdir(parents=True)
        worktree = directory / 'worktree'
        branch = 'ruby-loop/' + id
        files, _, source_hash = capture(self.root)
        base = git(self.root, 'rev-parse', 'HEAD')
        git(self.root, 'worktree', 'add', '-b', branch, str(worktree), base)
        # Include the current task's uncommitted fixes without changing the user's checkout.
        prior = capture(worktree)[0]
        for name in prior.keys() - files.keys(): (worktree / name).unlink()
        for name, data in files.items():
            path = worktree / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        if capture(self.root)[2] != source_hash: raise DriverError('Source changed while the loop snapshot was created')
        if changes(worktree):
            git(worktree, 'add', '-A')
            git(worktree, 'commit', '-m', 'Snapshot Ruby loop input (not implementation acceptance)')
        self.state = dict(loop_id=id, directory=str(directory), worktree=str(worktree), branch=branch,
                          root=str(self.root), base_head=base, accepted_head=git(worktree, 'rev-parse', 'HEAD'),
                          options=self.options, status='running', iteration=0, last_run=None,
                          task=None, feedback=[], stalls=0, created_at=now())
        self.state['trusted'] = self.trusted_manifest()
        self.state['driver_hashes'] = self.driver_manifest()
        self.save()
        self.emit(f'Loop {id}: branch {branch}; worktree {worktree}')

    def trusted_manifest(self):
        # Everything outside implementation/test files stays under integrator control.
        _, manifest, _ = capture(self.worktree)
        return {name: hash for name, hash in manifest.items() if not name.startswith(('bindings/ruby/lib/', 'bindings/ruby/ext/', 'bindings/ruby/test/', 'bindings/ruby/examples/'))
                and name not in ('bindings/ruby/api-coverage.json', 'bindings/ruby/README.md', 'bindings/ruby/briefs/raw-records.md')}

    def assert_trusted(self):
        if self.trusted_manifest() != self.state['trusted']:
            raise DriverError('Protected source or verifier changed. Restore it before resuming; no candidate was accepted.')
        if self.driver_manifest() != self.state['driver_hashes']:
            raise DriverError('The original verifier or driver changed; start a new loop from the reviewed version')

    def driver_manifest(self):
        return {name: hash for name, hash in capture(self.root)[1].items() if name.startswith('tools/')}

    def integrate_main(self, head):
        """Fast-forward the user's main checkout after a verified milestone."""
        branch = git(self.root, 'branch', '--show-current')
        if branch != 'main':
            raise DriverError('Automatic integration requires the root checkout on main; found ' + (branch or '(detached HEAD)'))
        dirty = changes(self.root)
        if dirty:
            raise DriverError('Cannot integrate onto main with uncommitted changes: ' + ', '.join(dirty))
        if git(self.root, 'rev-parse', 'HEAD') != self.state['accepted_head']:
            raise DriverError('Main changed outside the driver; inspect it before integrating the accepted milestone')
        git(self.root, 'merge', '--ff-only', head)
        if git(self.root, 'rev-parse', 'HEAD') != head:
            raise DriverError('Main did not fast-forward to the accepted milestone')

    def commit_wip(self, brief, implementation, agent):
        """Persist and mirror each scoped implementation attempt before verification."""
        changed = changes(self.worktree)
        if not changed:
            return self.state['accepted_head']
        git(self.worktree, 'add', '--', *changed)
        message = (f"WIP Ruby milestone: {Path(brief).stem}\n\n"
                   f"Agent: {agent.get('thread_id')}\n"
                   f"Summary: {implementation.get('summary', '')}\n")
        git(self.worktree, 'commit', '-F', '-', input=message.encode())
        head = git(self.worktree, 'rev-parse', 'HEAD')
        self.integrate_main(head)
        self.state['accepted_head'] = head
        self.save()
        self.emit('WIP committed and mirrored to main at ' + head[:12])
        return head

    def assert_evidence_current(self):
        row = self.store.db.execute('SELECT * FROM runs WHERE run_id=?', (self.state['last_run'],)).fetchone()
        if not row or row['status'] != 'completed' or row['source_hash'] != capture(self.worktree)[2] or row['catalog_hash'] != digest(build(self.worktree)) or row['environment_hash'] != environment(self.options['ruby'])[1]:
            raise DriverError('Verification evidence changed during review; resume to run fresh verification')

    def verify(self, context, artifact, brief=None):
        self.assert_trusted()
        context_file = artifact / 'context.json'
        context_file.write_text(json.dumps(context))
        args = [sys.executable, str(self.root / 'tools/verify_ruby.py'), '--root', str(self.worktree), '--db', str(self.store.path),
                'run', '--agent', context['agent'], '--model', self.options['model'], '--reasoning', self.options['reasoning'],
                '--task', context['task'], '--iteration', str(self.state['iteration']), '--context-file', str(context_file),
                '--jobs', str(self.options['jobs']), '--ruby', self.options['ruby']]
        if brief:
            args += ['--brief', brief]
        result = stream(args, self.worktree, artifact / 'verification', self.emit, self.options['verify_timeout'])
        evidence = result['last_json']
        if result['exit_code'] not in (0, 1) or not isinstance(evidence, dict) or 'run_id' not in evidence:
            raise DriverError('Verification did not produce a complete run; inspect its log')
        row = self.store.db.execute('SELECT * FROM runs WHERE run_id=?', (evidence['run_id'],)).fetchone()
        if not row or row['status'] != 'completed': raise DriverError('Verification run is incomplete')
        checks = build(self.worktree)
        expected = select_checks(checks, brief) if brief else checks
        if row['source_hash'] != capture(self.worktree)[2] or row['catalog_hash'] != digest(checks) or row['environment_hash'] != environment(self.options['ruby'])[1]:
            raise DriverError('Verification evidence is stale')
        rows = [dict(r) for r in self.store.db.execute('SELECT * FROM results WHERE run_id=?', (row['run_id'],))]
        if {r['check_id'] for r in rows} != {c['id'] for c in expected}: raise DriverError('Run omitted required checks')
        if any(r['status'] not in ('pass', 'fail', 'error') for r in rows): raise DriverError('Run contains unfinished checks')
        self.state['last_run'] = row['run_id']
        self.save()
        self.emit('Verification ' + row['run_id'] + ': ' + json.dumps(evidence['counts']))
        return rows

    def agent(self, prompt, artifact, review=False):
        schema = REVIEW_SCHEMA if review else IMPLEMENT_SCHEMA
        schema_path, report_path = artifact / 'schema.json', artifact / 'report.json'
        schema_path.write_text(json.dumps(schema))
        (artifact / 'prompt.md').write_text(prompt)
        args = [self.options['codex'], 'exec', '--json', '--color', 'never',
                '--sandbox', 'read-only' if review else 'workspace-write',
                '-c', 'approval_policy="never"', '-c', 'model_reasoning_effort=' + json.dumps(self.options['review_reasoning'] if review else self.options['reasoning']),
                '--model', self.options['review_model'] if review else self.options['model'],
                '--cd', str(self.worktree), '--output-schema', str(schema_path), '--output-last-message', str(report_path), '-']
        summary = stream(args, self.worktree, artifact / 'codex', self.emit, self.options['agent_timeout'], prompt, True)
        try: report = json.loads(report_path.read_text())
        except (OSError, ValueError) as error: raise DriverError('Agent final report is missing or malformed') from error
        if review: validate_review(report)
        elif (not isinstance(report, dict) or set(report) != {'summary', 'blockers'} or
              not isinstance(report['summary'], str) or not isinstance(report['blockers'], list) or
              not all(isinstance(x, str) for x in report['blockers'])):
            raise DriverError('Implementation report is malformed')
        summary['model'] = self.options['review_model'] if review else self.options['model']
        summary['reasoning'] = self.options['review_reasoning'] if review else self.options['reasoning']
        return report, summary

    def scope(self, brief):
        allowed = owned_files(self.worktree, brief)
        illegal = [p for p in changes(self.worktree) if not any(fnmatch.fnmatchcase(p, pattern) for pattern in allowed)]
        if illegal: raise DriverError('Changes outside the assignment: ' + ', '.join(illegal))
        if git(self.worktree, 'rev-parse', 'HEAD') != self.state['accepted_head']:
            raise DriverError('The agent committed or changed HEAD; only the driver can commit')
        ledger = 'bindings/ruby/api-coverage.json'
        old = json.loads(git(self.worktree, 'show', 'HEAD:' + ledger))['entries']
        new = json.loads((self.worktree / ledger).read_text())['entries']
        if {r['symbol'] for r in old} != {r['symbol'] for r in new}: raise DriverError('The agent changed the API inventory')
        old = {r['symbol']: r for r in old}
        for entry in new:
            previous = old[entry['symbol']]
            if entry == previous: continue
            if previous['brief'] != brief and brief != BASE + '99-integration.md':
                raise DriverError('The agent changed another assignment in the API ledger')
            if any(previous[key] != entry.get(key) for key in ('brief', 'symbol', 'c_signature')):
                raise DriverError('The agent changed the C API assignment contract')

    def implementation_prompt(self, brief, rows):
        failed = [{k: r.get(k) for k in ('check_id', 'status', 'message')} for r in rows if r['check_id'] in task_ids(build(self.worktree), brief) and r['status'] != 'pass']
        return f'''Implement the Ruby binding assignment in {brief}.
Read bindings/ruby/AGENTS.md and bindings/ruby/briefs/CONTRACT.md.
Read the assignment and its dependency briefs.
Implement the real native behavior described by the assignment.
Use private public-C-API fixtures when a later wrapper is unavailable.
Do not create a fake scheduler, dummy result, or placeholder implementation.
Treat every P1 and P2 finding from prior reviews as required work.
Prioritize completing the assignment's acceptance requirements over merely making
the current verifier checks pass.
Work through all actionable requirements and prior review findings in this turn.
Implement complete, production-quality behavior and tests within the allowed scope.
Do not report an item as a blocker if you can resolve it by inspecting the source,
the public C API, or existing tests. Only stop for genuinely external blockers.
Change only these paths or patterns:
{json.dumps(owned_files(self.worktree, brief), indent=2)}
Explain each necessary shared registration change in your report.
Run focused tests with this Ruby executable: {self.options['ruby']}.
Fix focused-test failures before returning, and add the required private
public-C-API fixtures and behavioral coverage when the assignment calls for them.
Do not change the verifier, its probes, briefs, task assignments, or check database.
Do not fabricate loaded features or test evidence.
Do not remove assertions to make a test pass.
Do not commit, reset, rebase, merge, push, or start other agents.
The driver runs full verification after your turn.
This assignment is a milestone, not completion of the binding.
The final integration gate still requires every deferred runtime probe.
Continue until the actionable requirements for this assignment are implemented
and tested. Return a summary of the completed changes and only concrete blockers
that are genuinely external to the repository or unavailable runtime.
Latest verification run: {self.state['last_run']}
Current failed milestone checks:
{json.dumps(failed, indent=2)}
Feedback from prior attempts:
{json.dumps(self.state['feedback'], indent=2)}
'''

    def review_prompt(self, brief, full=False):
        return f'''Review the Ruby binding {'in full' if full else 'changes against HEAD'}.
The current assignment is {brief}.
Read bindings/ruby/AGENTS.md, the shared contract, and the full assignment.
Inspect actual source and tests. Do not trust the implementation report.
Check native ownership, GC compaction, callback expiry, status conversion, and sender routing where applicable.
Reject placeholders, discarded fields, fabricated test evidence, and weakened tests.
For a milestone, inspect every requirement implementable with its dependencies.
Treat missing acceptance coverage as a defect even when the scoped verifier checks
pass. Specifically require the assignment's native fixtures, lifetime/GC behavior,
callback and reentry safety, overflow handling, and cleanup coverage where listed.
Identify concrete defects with file paths and explanations.
Do not demand later wrappers when a private public-C-API fixture establishes behavior.
For final integration, inspect the complete binding against all briefs.
The full check run is {self.state['last_run']}.
The verification database is {self.store.path}. Read it without changing it.
Do not edit files, commit, start agents, or modify the verification database.
Approve only if there are no unresolved findings.
'''

    def run(self):
        checks = build(self.worktree)
        directory = Path(self.state['directory'])
        if self.state.get('last_run') and self.state.get('iteration', 0) > 0:
            protected = set(self.state.get('protected_passes', []))
            rows = [dict(check_id=check['id'], status='pass' if check['id'] in protected else 'fail',
                         message='Resumed from protected pass ledger') for check in checks]
            self.emit('Resumed from protected pass ledger; skipping the full baseline run')
        else:
            baseline_dir = directory / ('baseline-' + uuid.uuid4().hex[:8])
            baseline_dir.mkdir()
            context = dict(agent='driver/' + self.state['loop_id'], task='baseline', loop_id=self.state['loop_id'])
            rows = self.verify(context, baseline_dir)
            self.state['protected_passes'] = sorted(set(self.state.get('protected_passes', [])) |
                                                    {r['check_id'] for r in rows if r['status'] == 'pass'})
        self.state['status'] = 'running'
        self.save()
        attempted = 0
        while True:
            all_pass = all(row['status'] == 'pass' for row in rows)
            brief = self.state['task'] or next_task(checks, rows)
            if all_pass and brief is None:
                # Even an already-green starting checkout needs whole-binding review.
                artifact = directory / ('final-review-' + uuid.uuid4().hex[:8])
                artifact.mkdir()
                source_hash = capture(self.worktree)[2]
                report, reviewer = self.agent(self.review_prompt(BASE + '99-integration.md', True), artifact, True)
                self.assert_trusted()
                if capture(self.worktree)[2] != source_hash: raise DriverError('The reviewer changed source')
                self.assert_evidence_current()
                self.state['final_review'] = dict(report=report, reviewer=reviewer)
                if report['verdict'] == 'approve':
                    self.state['status'] = 'complete'
                    self.save()
                    self.emit(f"DONE: every check passed; final review approved; branch {self.state['branch']}")
                    return 0
                self.state['task'] = brief = BASE + '99-integration.md'
                self.state['feedback'] = report['findings']
                self.save()
            if self.options['max_iterations'] and attempted >= self.options['max_iterations']:
                self.state['status'] = 'paused'
                self.save()
                self.emit('Iteration limit reached; resume loop ' + self.state['loop_id'])
                return 1
            if self.state['stalls'] >= self.options['stall_limit']:
                raise DriverError('Repeated attempts made no verified progress. Inspect feedback, then resume the retained loop.')
            if not brief: raise DriverError('No assignment covers the remaining failures')
            self.state['task'] = brief
            self.state['iteration'] += 1
            number = self.state['iteration']
            attempted += 1
            artifact = directory / f'iteration-{number:04}'
            artifact.mkdir()
            self.save()
            record = dict(number=number, brief=brief, status='implementing', before_run=self.state['last_run'], started_at=now(), artifact=str(artifact))
            self.iteration_record(record)
            self.emit(f'Iteration {number}: {brief}')
            implementation, agent = self.agent(self.implementation_prompt(brief, rows), artifact)
            record.update(implementation=implementation, agent=agent)
            self.iteration_record(record)
            self.assert_trusted()
            self.scope(brief)
            self.commit_wip(brief, implementation, agent)
            new_rows = self.verify(dict(agent=agent.get('thread_id') or 'driver/' + self.state['loop_id'],
                task=brief, loop_id=self.state['loop_id'], previous_run=record['before_run'],
                implementation=implementation, codex=agent), artifact, brief)
            selected = task_ids(checks, brief)
            protected = set(self.state['protected_passes'])
            missing, regressions = acceptance([dict(check_id=id, status='pass') for id in protected], new_rows, selected)
            old_pass = {r['check_id'] for r in rows if r['status'] == 'pass'}
            new_pass = {r['check_id'] for r in new_rows if r['status'] == 'pass'}
            self.state['protected_passes'] = sorted(set(self.state['protected_passes']) | new_pass)
            improved = new_pass > protected
            self.emit(f'Progress: {len(new_pass)}/{len(checks)} passing; {len(new_pass-old_pass)} new passes; {len(regressions)} regressions')
            record.update(after_run=self.state['last_run'], missing=missing, regressions=regressions)
            feedback = []
            if missing: feedback.append({'missing_checks': missing})
            if regressions: feedback.append({'regressions': regressions})
            if not missing and not regressions:
                review_dir = artifact / 'review'
                review_dir.mkdir()
                source_hash = capture(self.worktree)[2]
                report, reviewer = self.agent(self.review_prompt(brief), review_dir, True)
                record.update(review=report, reviewer=reviewer)
                self.assert_trusted()
                self.scope(brief)
                if capture(self.worktree)[2] != source_hash: raise DriverError('The reviewer changed verified source')
                self.assert_evidence_current()
                if report['verdict'] == 'approve':
                    changed = changes(self.worktree)
                    if changed:
                        git(self.worktree, 'add', '--', *changed)
                        # Commits record the exact verified source. Hooks are checked after commit.
                        message = f"Implement Ruby milestone: {Path(brief).stem}\n\nVerification: {self.state['last_run']}\nAgent: {agent.get('thread_id')}\nReviewer: {reviewer.get('thread_id')}\n"
                        git(self.worktree, 'commit', '-F', '-', input=message.encode())
                        if changes(self.worktree) or capture(self.worktree)[2] != source_hash:
                            raise DriverError('A commit hook changed verified source; inspect the retained worktree')
                    accepted_head = git(self.worktree, 'rev-parse', 'HEAD')
                    self.integrate_main(accepted_head)
                    self.state['accepted_head'] = accepted_head
                    record.update(status='accepted', commit=self.state['accepted_head'])
                    self.state.update(task=None, feedback=[], stalls=0)
                    self.emit('Accepted milestone at ' + self.state['accepted_head'][:12])
                else:
                    feedback += report['findings']
            if record.get('status') != 'accepted':
                record['status'] = 'repair_required'
                self.state['feedback'] = feedback + [{'agent_blockers': implementation['blockers']}]
                self.state['stalls'] = 0 if improved else self.state['stalls'] + 1
                self.emit('Candidate retained for repair; WIP commit remains available for repair')
            record['finished_at'] = now()
            self.iteration_record(record)
            self.save()
            rows = new_rows
