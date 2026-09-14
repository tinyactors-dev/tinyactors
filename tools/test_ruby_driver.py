#!/usr/bin/env python3
"""Driver tests use real Git/SQLite/processes and synthetic agents, with no model calls."""
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from ruby_verify.catalog import BASE, build, owner
from ruby_verify.loop import Driver, DriverError, ORDER, acceptance, changes, git, next_task, owned_files, stream, task_ids, validate_review
from ruby_verify.store import Store

ROOT = Path(__file__).resolve().parent.parent
BRIEF = BASE + '00-bootstrap.md'
CHECK = dict(id='fixture.check', brief=BRIEF, kind='ruby')
APPROVE = dict(verdict='approve', summary='Reviewed source and tests', findings=[])
REJECT = dict(verdict='changes_requested', summary='Needs correction', findings=[dict(priority='P1', path='bindings/ruby/lib/fix.rb', detail='Missing native behavior')])
OPTIONS = dict(model='gpt-5.6-luna', reasoning='low', review_model='gpt-5.6-sol', review_reasoning='medium',
               codex='codex', ruby='ruby', jobs=1, max_iterations=0, stall_limit=3, agent_timeout=5, verify_timeout=5)


class DriverTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.root = self.path / 'repo'
        self.root.mkdir()
        git(self.root, 'init', '-q')
        git(self.root, 'config', 'user.name', 'Driver Test')
        git(self.root, 'config', 'user.email', 'driver-test@example.invalid')
        files = {
            'README.md': 'fixture\n', '.gitignore': 'bindings/ruby/.verification/\n',
            BRIEF: '# Fixture\n## Owned files\n- `lib/fix.rb`\n## Required behavior\nImplement behavior.\n',
            'bindings/ruby/api-coverage.json': '{"entries":[]}',
            'bindings/ruby/lib/fix.rb': 'missing\n', 'tools/protected.py': '# verifier\n'}
        for name, data in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(data)
        git(self.root, 'add', '.')
        git(self.root, 'commit', '-qm', 'Initial fixture')
        self.store = Store(self.path / 'data/checks.sqlite3')
        self.addCleanup(self.store.close)

    def create(self, cls=Driver, **options):
        driver = cls(self.root, self.store, dict(OPTIONS, **options))
        with patch('ruby_verify.loop.build', return_value=[CHECK]), redirect_stdout(io.StringIO()):
            driver.initialize()
        return driver

    def test_models_are_distinct(self):
        self.assertEqual('gpt-5.6-luna', OPTIONS['model'])
        self.assertEqual('gpt-5.6-sol', OPTIONS['review_model'])

    def test_codex_command_uses_selected_models_permissions_and_schema(self):
        driver = self.create()
        fake = self.path / 'fake-codex'
        fake.write_text('#!' + sys.executable + '\n' + '''import json,sys
from pathlib import Path
args=sys.argv[1:]
out=Path(args[args.index('--output-last-message')+1])
out.with_suffix('.argv').write_text(json.dumps(args))
review=args[args.index('--sandbox')+1]=='read-only'
report={'verdict':'approve','summary':'Reviewed','findings':[]} if review else {'summary':'Implemented','blockers':[]}
out.write_text(json.dumps(report))
print(json.dumps({'type':'thread.started','thread_id':'fake-session'}))
print(json.dumps({'type':'turn.completed','usage':{'output_tokens':7}}))
''')
        fake.chmod(0o755)
        driver.options['codex'] = str(fake)
        for review, model, sandbox in [(False, 'gpt-5.6-luna', 'workspace-write'), (True, 'gpt-5.6-sol', 'read-only')]:
            artifact = self.path / str(review)
            artifact.mkdir()
            report, context = driver.agent('Execute one test fixture.', artifact, review)
            args = json.loads((artifact / 'report.argv').read_text())
            self.assertEqual(model, args[args.index('--model')+1])
            self.assertEqual(sandbox, args[args.index('--sandbox')+1])
            self.assertEqual('fake-session', context['thread_id'])
            self.assertEqual(7, context['usage']['output_tokens'])
            self.assertIn('--output-schema', args)

    def test_changing_original_driver_invalidates_loop(self):
        driver = self.create()
        (self.root / 'tools/protected.py').write_text('changed driver')
        with self.assertRaisesRegex(DriverError, 'original verifier'): driver.assert_trusted()

    def test_resume_can_change_explicit_iteration_limit(self):
        driver = self.create(max_iterations=1)
        resumed = Driver(self.root, self.store, OPTIONS)
        with redirect_stdout(io.StringIO()):
            resumed.initialize(driver.state['loop_id'], {'max_iterations': 4})
        self.assertEqual(4, resumed.options['max_iterations'])
        self.assertEqual('gpt-5.6-sol', resumed.options['review_model'])

    def test_every_brief_has_a_milestone(self):
        self.assertEqual({c['brief'] for c in build(ROOT)}, set(ORDER))
        self.assertEqual(len(ORDER), len(set(ORDER)))

    def test_runtime_cycles_are_deferred_but_full_gate_retains_them(self):
        checks = build(ROOT)
        self.assertNotIn('behavior.runtime.bytes', task_ids(checks, owner('raw.Memory')))
        self.assertIn('behavior.runtime.bytes', task_ids(checks, BASE + '99-integration.md'))
        self.assertEqual({c['id'] for c in checks}, task_ids(checks, BASE + '99-integration.md'))

    def test_selector_ignores_previous_prose_and_requires_passes(self):
        self.assertEqual(BRIEF, next_task([CHECK], [dict(check_id=CHECK['id'], status='stale')]))
        self.assertIsNone(next_task([CHECK], [dict(check_id=CHECK['id'], status='pass')]))

    def test_regressions_prevent_acceptance_despite_more_total_passes(self):
        before = [dict(check_id='old', status='pass')]
        after = [dict(check_id='old', status='fail'), dict(check_id='new1', status='pass'), dict(check_id='new2', status='pass')]
        missing, regressions = acceptance(before, after, {'new1', 'new2'})
        self.assertEqual([], missing)
        self.assertEqual(['old'], regressions)

    def test_stale_selected_check_is_not_accepted(self):
        self.assertEqual((['target'], []), acceptance([], [dict(check_id='target', status='stale')], {'target'}))

    def test_approval_requires_well_formed_empty_findings(self):
        self.assertEqual(APPROVE, validate_review(APPROVE))
        for value in [None, {}, dict(APPROVE, verdict='maybe'), dict(APPROVE, findings=REJECT['findings']), dict(REJECT, findings=[])]:
            with self.assertRaises(DriverError): validate_review(value)

    def test_snapshot_includes_uncommitted_input_without_changing_original(self):
        file = self.root / 'bindings/ruby/lib/fix.rb'
        file.write_text('user edits\n')
        original_head = git(self.root, 'rev-parse', 'HEAD')
        driver = self.create()
        self.assertEqual('user edits\n', (driver.worktree / 'bindings/ruby/lib/fix.rb').read_text())
        self.assertEqual(original_head, git(self.root, 'rev-parse', 'HEAD'))
        self.assertEqual(['bindings/ruby/lib/fix.rb'], changes(self.root))
        self.assertEqual([], changes(driver.worktree))
        self.assertIn('Snapshot Ruby loop input', git(driver.worktree, 'log', '-1', '--format=%s'))

    def test_protected_source_change_stops_before_verification(self):
        driver = self.create()
        (driver.worktree / 'tools/protected.py').write_text('weakened verifier')
        with self.assertRaisesRegex(DriverError, 'Protected'): driver.assert_trusted()

    def test_untracked_out_of_scope_file_is_rejected(self):
        driver = self.create()
        (driver.worktree / 'bindings/ruby/lib/unrelated.rb').write_text('unrelated')
        with self.assertRaisesRegex(DriverError, 'outside'): driver.scope(BRIEF)

    def test_agent_commit_is_rejected(self):
        driver = self.create()
        git(driver.worktree, 'commit', '--allow-empty', '-qm', 'Agent bypass')
        with self.assertRaisesRegex(DriverError, 'only the driver'): driver.scope(BRIEF)

    def test_resume_keeps_candidate_and_iteration_context(self):
        driver = self.create()
        file = driver.worktree / 'bindings/ruby/lib/fix.rb'
        file.write_text('unfinished work\n')
        driver.state.update(iteration=7, feedback=['repair needed'], status='interrupted', protected_passes=['already-passed'])
        driver.save()
        resumed = Driver(self.root, self.store, OPTIONS)
        resumed.initialize(driver.state['loop_id'])
        self.assertEqual(7, resumed.state['iteration'])
        self.assertEqual(['already-passed'], resumed.state['protected_passes'])
        self.assertEqual('unfinished work\n', file.read_text())

    def test_stream_requires_turn_completion_not_exit_zero(self):
        with self.assertRaisesRegex(DriverError, 'did not complete'):
            stream([sys.executable, '-c', 'print("{}")'], self.root, self.path / 'empty', lambda _: None, 3, codex=True)

    def test_stream_captures_identity_usage_and_agent_progress(self):
        events = [{'type':'thread.started','thread_id':'test-session'},
                  {'type':'item.completed','item':{'type':'agent_message','text':'Implemented a fixture'}},
                  {'type':'turn.completed','usage':{'input_tokens':17}}]
        code = 'import json; events=' + repr(events) + '; [print(json.dumps(e),flush=True) for e in events]'
        messages = []
        result = stream([sys.executable, '-c', code], self.root, self.path / 'events', messages.append, 3, codex=True)
        self.assertEqual('test-session', result['thread_id'])
        self.assertEqual({'input_tokens':17}, result['usage'])
        self.assertTrue(any('Implemented' in m for m in messages))
        self.assertTrue((self.path / 'events.stdout').is_file())

    def test_stream_timeout_kills_process(self):
        with self.assertRaisesRegex(DriverError, 'timed out'):
            stream([sys.executable, '-c', 'import time; time.sleep(10)'], self.root, self.path / 'timeout', lambda _: None, .1)

    def test_stream_large_prompt_does_not_deadlock(self):
        code = 'import sys,json; print("x"*100000,flush=True); data=sys.stdin.read(); print(json.dumps({"type":"turn.completed","usage":{"bytes":len(data)}}),flush=True)'
        result = stream([sys.executable, '-c', code], self.root, self.path / 'prompt', lambda _: None, 3, 'p'*100000, True)
        self.assertEqual(100000, result['usage']['bytes'])

    def test_real_scope_rules_cover_assigned_files(self):
        for brief in ORDER:
            patterns = owned_files(ROOT, brief)
            self.assertTrue(patterns)
            self.assertFalse(any(p.startswith('tools/') for p in patterns))

    def test_reviewer_rejection_repairs_before_commit_and_finishes(self):
        class Simulated(Driver):
            reviews = 0
            implementations = 0
            def assert_evidence_current(self): pass # Synthetic checks have no native verification run.
            def verify(self, context, artifact, brief=None):
                self.state['last_run'] = 'synthetic-' + str(self.state['iteration'])
                status = 'pass' if (self.worktree / 'bindings/ruby/lib/fix.rb').read_text() != 'missing\n' else 'fail'
                return [dict(check_id=CHECK['id'], status=status, message='Synthetic verifier fixture')]
            def agent(self, prompt, artifact, review=False):
                identity = dict(thread_id='reviewer' if review else 'implementer')
                if review:
                    self.reviews += 1
                    return (REJECT if self.reviews == 1 else APPROVE), identity
                self.implementations += 1
                (self.worktree / 'bindings/ruby/lib/fix.rb').write_text('version '+str(self.implementations)+'\n')
                return dict(summary='Implemented fixture', blockers=[]), identity
        driver = self.create(Simulated)
        with patch('ruby_verify.loop.build', return_value=[CHECK]), redirect_stdout(io.StringIO()):
            self.assertEqual(0, driver.run())
        self.assertEqual(2, driver.implementations)
        self.assertEqual('complete', driver.state['status'])
        # Accepted milestones are fast-forwarded onto the root main checkout.
        self.assertEqual('version 2\n', (self.root / 'bindings/ruby/lib/fix.rb').read_text())
        commits = git(driver.worktree, 'log', '--format=%s').splitlines()
        self.assertEqual(2, sum('WIP Ruby milestone' in c for c in commits))
        records = [json.loads(r[0]) for r in self.store.db.execute('SELECT record_json FROM driver_iterations ORDER BY number')]
        self.assertEqual(['repair_required', 'accepted'], [r['status'] for r in records])
        self.assertEqual('changes_requested', records[0]['review']['verdict'])
        self.assertEqual('reviewer', records[1]['reviewer']['thread_id'])

    def test_stall_retains_work_and_never_claims_completion(self):
        class Stalled(Driver):
            def verify(self, context, artifact, brief=None):
                self.state['last_run']='synthetic'
                return [dict(check_id=CHECK['id'], status='fail', message='not implemented')]
            def agent(self, prompt, artifact, review=False):
                return dict(summary='No progress', blockers=['Need an implementation']), dict(thread_id='fixture')
        driver = self.create(Stalled, stall_limit=2)
        with patch('ruby_verify.loop.build', return_value=[CHECK]), redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(DriverError, 'no verified progress'): driver.run()
        self.assertEqual(2, driver.state['iteration'])
        self.assertNotEqual('complete', driver.state['status'])
        self.assertEqual('Initial fixture', git(driver.worktree, 'log', '-1', '--format=%s'))


if __name__ == '__main__': unittest.main(verbosity=2)
