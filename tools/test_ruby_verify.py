#!/usr/bin/env python3
"""Harness regression tests. Synthetic fixtures test the verifier, never binding acceptance."""
from contextlib import redirect_stdout
import fcntl
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from ruby_verify.catalog import build, digest
from ruby_verify.runner import Harness, capture, environment, evidence_status, process, recover, select
from ruby_verify.store import Store, now

ROOT = Path(__file__).resolve().parent.parent
RUBY = shutil.which('ruby')
if '--ruby' in sys.argv:
    index = sys.argv.index('--ruby')
    RUBY = sys.argv[index + 1]
    del sys.argv[index:index + 2]
SPEC = dict(id='fixture.assertion', revision='v1', title='Harness fixture', brief='fixture.md', kind='ruby', probe='class', klass='raw.Example')


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.store = Store(self.path / 'evidence/checks.sqlite3')
        self.addCleanup(self.store.close)
        self.store.sync([SPEC])

    def run_record(self, id='run1'):
        return dict(run_id=id, started_at=now(), status='running', agent='/root/fixture', model='test-model',
                    reasoning='low', task='verify-the-verifier', iteration='2', context_json='{"previous":"run0"}',
                    scope_json='{}', source_hash='source', catalog_hash='catalog', environment_hash='env',
                    environment_json='{}', source_manifest_json='{}', artifact_dir=str(self.path / id), pid=os.getpid())

    def start(self, id='run1'):
        self.store.begin_run(self.run_record(id), [SPEC])

    def source(self):
        root = self.path / 'source'
        (root / 'bindings/ruby/lib').mkdir(parents=True, exist_ok=True)
        (root / 'bindings/ruby/lib/tinyactors.rb').write_text('module TinyActors; module Raw; class Example; end; end; end\n')
        (root / 'tools').mkdir(exist_ok=True)
        shutil.copytree(ROOT / 'tools/ruby_verify', root / 'tools/ruby_verify', dirs_exist_ok=True, ignore=shutil.ignore_patterns('__pycache__'))
        return root

    def worker(self, body=None, check=None, timeout=5):
        root = self.source()
        if body is not None:
            (root / 'tools/ruby_verify/worker.rb').write_text(body)
        harness = Harness(root, self.store, RUBY, timeout=timeout)
        return harness.ruby_check(check or SPEC, root, self.path / 'log')

    def protocol(self, **fields):
        base = dict(status='pass', assertions=1, message='fixture', skips=0)
        base.update(fields)
        return "require 'json'; s=JSON.parse(File.read(ARGV[0])); r=JSON.parse(" + repr(json.dumps(base)) + "); r['nonce']=s['nonce']; r['check_id']=s['check']['id']; File.write(s['result_path'],JSON.generate(r))"

    def suite(self, body):
        root = self.source()
        file = root / 'bindings/ruby/test/fixture_test.rb'
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(body)
        check = dict(SPEC, kind='agent_suite', files=['bindings/ruby/test/fixture_test.rb'])
        return Harness(root, self.store, RUBY).ruby_check(check, root, self.path / 'suite')


class StoreTests(Fixture):
    def test_history_preserves_context_and_prior_failure(self):
        self.start()
        self.store.record('run1', SPEC['id'], dict(status='fail', message='Missing method', finished_at=now()))
        self.store.finish('run1', 'completed', 1)
        time.sleep(.005)
        self.start('run2')
        self.store.record('run2', SPEC['id'], dict(status='pass', message='Asserted', assertions=3, finished_at=now()))
        self.store.finish('run2', 'completed', 0)
        rows = self.store.history(SPEC['id'])
        self.assertEqual(['pass', 'fail'], [r['status'] for r in rows])
        self.assertEqual('/root/fixture', rows[0]['agent'])
        self.assertEqual('low', rows[0]['reasoning'])
        self.assertEqual({'previous': 'run0'}, json.loads(rows[0]['context_json']))

    def test_incomplete_attempt_cannot_pass(self):
        self.start()
        self.store.record('run1', SPEC['id'], {'status': 'pass', 'assertions': 1})
        self.assertEqual('incomplete', evidence_status(self.store.latest(SPEC['id']), SPEC, 'source', 'catalog', 'env'))

    def test_all_fingerprints_invalidate_pass(self):
        self.start()
        self.store.record('run1', SPEC['id'], {'status': 'pass', 'assertions': 1})
        self.store.finish('run1', 'completed', 0)
        row = self.store.latest(SPEC['id'])
        self.assertEqual('pass', evidence_status(row, SPEC, 'source', 'catalog', 'env'))
        for key in ('revision', 'source_hash', 'catalog_hash', 'environment_hash'):
            self.assertEqual('stale', evidence_status(dict(row, **{key: 'changed'}), SPEC, 'source', 'catalog', 'env'))

    def test_catalog_deletion_rejected_and_revisions_retained(self):
        with self.assertRaisesRegex(RuntimeError, 'removed required'):
            self.store.sync([])
        self.store.sync([dict(SPEC, revision='v2', title='New spec')])
        self.assertEqual(2, self.store.db.execute('SELECT count(*) FROM check_versions').fetchone()[0])

    def test_recover_abandoned_run_marks_pending_error(self):
        self.start()
        recover(self.store)
        self.assertEqual('error', self.store.latest(SPEC['id'])['status'])
        self.assertEqual('interrupted', self.store.latest(SPEC['id'])['run_status'])

    def test_recovery_leaves_locked_live_run_alone(self):
        self.start()
        locks = self.store.path.parent / 'locks'
        locks.mkdir()
        with (locks / 'run1.lock').open('w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            recover(self.store)
            self.assertEqual('running', self.store.latest(SPEC['id'])['run_status'])

    def test_transaction_rolls_back(self):
        with self.assertRaises(ValueError):
            with self.store.transaction():
                self.store.set_setting('never', 'committed')
                raise ValueError('rollback')
        self.assertIsNone(self.store.setting('never'))

    def test_concurrent_connections_keep_both_attempts(self):
        failures = []
        def write(id):
            other = Store(self.store.path)
            try:
                other.begin_run(self.run_record(id), [SPEC])
                other.record(id, SPEC['id'], {'status': 'fail', 'message': id})
                other.finish(id, 'completed', 1)
            except Exception as exc:
                failures.append(exc)
            finally:
                other.close()
        threads = [threading.Thread(target=write, args=(str(i),)) for i in range(4)]
        for thread in threads: thread.start()
        for thread in threads: thread.join()
        self.assertEqual([], failures)
        self.assertEqual(4, self.store.db.execute('SELECT count(*) FROM results').fetchone()[0])


class ProcessTests(Fixture):
    def child(self, code, timeout=3):
        return process([sys.executable, '-c', code], self.path, self.path / 'process', timeout)

    def test_timeout(self):
        result = self.child('import time; time.sleep(20)', .1)
        self.assertEqual('timeout', result['reason'])
        self.assertNotEqual(0, result['exit_code'])

    def test_signal_crash(self):
        result = self.child('import os,signal; os.kill(os.getpid(),signal.SIGKILL)')
        self.assertEqual(-signal.SIGKILL, result['exit_code'])

    def test_output_limit(self):
        result = self.child('import sys,time; sys.stdout.write("x"*2000000); sys.stdout.flush(); time.sleep(2)')
        self.assertEqual('output_limit', result['reason'])
        self.assertLess(len(result['stdout']), 17000)

    def test_successful_parent_does_not_leave_descendant(self):
        marker = self.path / 'orphan'
        code = 'import subprocess,sys; subprocess.Popen([sys.executable,"-c",' + repr(f'import time; from pathlib import Path; time.sleep(.5); Path({str(marker)!r}).write_text("orphan")') + '])'
        self.assertEqual(0, self.child(code)['exit_code'])
        time.sleep(.7)
        self.assertFalse(marker.exists())

    def test_launch_failure(self):
        result = process(['/no/such/executable'], self.path, self.path / 'missing', 1)
        self.assertEqual('launch_error', result['reason'])


class ProtocolTests(Fixture):
    def test_actual_probe_can_pass_with_assertions(self):
        result = self.worker()
        self.assertEqual('pass', result['status'], result)
        self.assertGreater(result['assertions'], 0)

    def test_absent_class_is_failure(self):
        result = self.worker(check=dict(SPEC, klass='raw.Absent'))
        self.assertEqual('fail', result['status'])
        self.assertIn('Absent', result['message'])

    def test_exit_zero_without_evidence_is_error(self):
        self.assertEqual('error', self.worker('exit 0')['status'])

    def test_zero_assertions_is_error(self):
        self.assertEqual('error', self.worker(self.protocol(assertions=0))['status'])

    def test_skip_cannot_pass(self):
        self.assertEqual('error', self.worker(self.protocol(skips=1))['status'])

    def test_nonzero_exit_cannot_pass(self):
        self.assertEqual('error', self.worker(self.protocol() + '; exit 7')['status'])

    def test_wrong_nonce_is_error(self):
        self.assertEqual('error', self.worker(self.protocol().replace("r['nonce']=s['nonce']", "r['nonce']='old'"))['status'])

    def test_malformed_evidence_is_error(self):
        self.assertEqual('error', self.worker("require 'json'; s=JSON.parse(File.read(ARGV[0])); File.write(s['result_path'],'garbage')")['status'])

    def test_unknown_probe_is_error(self):
        self.assertEqual('error', self.worker(check=dict(SPEC, probe='not_a_probe'))['status'])

    def test_native_crash_is_error(self):
        self.assertEqual('error', self.worker("Process.kill('KILL', Process.pid)")['status'])

    def test_timeout_is_error(self):
        self.assertEqual('timeout', self.worker('sleep 20', timeout=.1)['message'])

    def test_missing_suite_is_failure(self):
        result = self.worker(check=dict(SPEC, kind='agent_suite', files=['missing_test.rb']))
        self.assertEqual('fail', result['status'])
        self.assertIn('Missing executable', result['message'])

    def test_minitest_pass(self):
        result = self.suite('require "minitest/autorun"; class ExampleTest < Minitest::Test; def test_real; assert_equal 2,1+1; end; end')
        self.assertEqual('pass', result['status'], result)
        self.assertEqual(1, result['assertions'])

    def test_minitest_failure(self):
        self.assertEqual('fail', self.suite('require "minitest/autorun"; class ExampleTest < Minitest::Test; def test_real; assert_equal 3,1+1; end; end')['status'])

    def test_minitest_skip(self):
        self.assertEqual('fail', self.suite('require "minitest/autorun"; class ExampleTest < Minitest::Test; def test_real; skip "later"; end; end')['status'])

    def test_minitest_no_tests(self):
        self.assertEqual('fail', self.suite('require "minitest/autorun"')['status'])

    def test_minitest_no_assertions(self):
        self.assertEqual('fail', self.suite('require "minitest/autorun"; class ExampleTest < Minitest::Test; def test_empty; end; end')['status'])


class CatalogAndSnapshotTests(Fixture):
    def test_full_catalog_has_behaviors_and_every_brief(self):
        checks = build(ROOT)
        self.assertGreater(len(checks), 500)
        self.assertGreater(len([c for c in checks if c['id'].startswith('behavior.')]), 100)
        self.assertEqual(54, len([c for c in checks if c['id'].startswith('raw.export.')]))
        owners = {c['brief'] for c in checks}
        for path in (ROOT / 'bindings/ruby/briefs').rglob('*.md'):
            if path.name not in ('README.md', 'CONTRACT.md'):
                self.assertIn(str(path.relative_to(ROOT)), owners)

    def test_all_probe_names_resolve_and_ruby_parses(self):
        probes = {c['probe'] for c in build(ROOT) if c['kind'] == 'ruby'}
        special = {'native_loaded', 'constant', 'export', 'native_export', 'ledger_entry', 'class', 'method'}
        ruby = (ROOT / 'tools/ruby_verify/probes.rb').read_text() + (ROOT / 'tools/ruby_verify/runtime_probes.rb').read_text()
        for probe in probes - special:
            self.assertIn('def probe_' + probe + '(', ruby)
        for file in (ROOT / 'tools/ruby_verify').glob('*.rb'):
            result = subprocess.run([RUBY, '-c', str(file)], capture_output=True, text=True)
            self.assertEqual(0, result.returncode, result.stderr)

    def test_c_oracle_compiles_actual_constant_values(self):
        source = self.source()
        shutil.copytree(ROOT / 'src', source / 'src')
        harness = Harness(source, self.store, RUBY)
        harness.checks = build(ROOT)
        gate = next(c for c in harness.checks if c['id'] == 'gate.oracle')
        result = harness.gate(gate, source, self.path / 'oracle')
        self.assertEqual('pass', result['status'], result)
        self.assertEqual(2**64-1, harness.expected['TA_TIME_NEVER'])
        self.assertEqual(0, harness.expected['TA_OK'])
        self.assertEqual('http://www.w3.org/TR/scxml/#SCXMLEventProcessor', harness.expected['TA_SCXML_IOPROCESSOR'])

    def test_scope_adds_shared_and_rejects_typos(self):
        shared = dict(SPEC, id='gate.example', shared=True, brief='bootstrap.md')
        self.assertEqual(2, len(select([SPEC, shared], brief='fixture.md')))
        with self.assertRaises(ValueError): select([SPEC], brief='typo.md')
        with self.assertRaises(ValueError): select([SPEC], patterns=['fixture.*', 'typo.*'])

    def test_snapshot_tracks_source_but_ignores_binary_and_db(self):
        root = self.source()
        before = capture(root)[2]
        (root / 'bindings/ruby/lib/old.bundle').write_bytes(b'old binary')
        local = root / 'bindings/ruby/.verification'
        local.mkdir()
        (local / 'checks.sqlite3').write_text('local history')
        self.assertEqual(before, capture(root)[2])
        (root / 'bindings/ruby/lib/tinyactors.rb').write_text('changed')
        self.assertNotEqual(before, capture(root)[2])

    def test_external_source_symlink_rejected(self):
        root = self.source()
        (self.path / 'external.rb').write_text('outside snapshot')
        (root / 'bindings/ruby/lib/external.rb').symlink_to(self.path / 'external.rb')
        with self.assertRaises(RuntimeError): capture(root)

    def test_end_to_end_run_archives_snapshot_and_context(self):
        root = self.source()
        harness = Harness(root, self.store, RUBY)
        with patch('ruby_verify.runner.catalog', return_value=[SPEC]), redirect_stdout(io.StringIO()):
            self.assertEqual(0, harness.run(dict(agent='/root/check', task='fixture task', extra={'brief': 'fixture.md'})))
        row = self.store.latest(SPEC['id'])
        self.assertEqual('pass', row['status'])
        self.assertEqual('/root/check', row['agent'])
        self.assertEqual('fixture task', row['task'])
        archive = self.store.db.execute('SELECT artifact_dir FROM runs').fetchone()[0]
        self.assertTrue((Path(archive) / 'source.tar.gz').is_file())
        self.assertEqual('pass', evidence_status(row, SPEC, capture(root)[2], digest([SPEC]), harness.env_hash))
        (root / 'bindings/ruby/lib/tinyactors.rb').write_text('changed')
        self.assertEqual('stale', evidence_status(row, SPEC, capture(root)[2], digest([SPEC]), harness.env_hash))

    def test_test_mutating_captured_source_invalidates_run(self):
        root = self.source()
        worker = root / 'tools/ruby_verify/worker.rb'
        worker.write_text(self.protocol() + "; File.write(File.join(s['root'],'bindings/ruby/lib/tinyactors.rb'),'tampered')")
        harness = Harness(root, self.store, RUBY)
        with patch('ruby_verify.runner.catalog', return_value=[SPEC]), redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, 'modified verification source'):
                harness.run(dict(agent='fixture'))
        self.assertEqual('interrupted', self.store.latest(SPEC['id'])['run_status'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
