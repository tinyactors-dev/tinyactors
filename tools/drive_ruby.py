#!/usr/bin/env python3
"""Run Luna implementation and Sol review until Ruby verification is complete."""
import argparse
import fcntl
import json
from pathlib import Path
import shutil
import signal
import sys

from ruby_verify.catalog import build
from ruby_verify.loop import Driver, DriverError, ORDER, task_ids
from ruby_verify.runner import environment, recover
from ruby_verify.store import Store


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    p.add_argument('--db', type=Path)
    p.add_argument('--resume', metavar='LOOP_ID')
    p.add_argument('--dry-run', action='store_true', help='Print configuration and milestones without starting agents or creating a worktree')
    p.add_argument('--ruby', help='Default: Ruby configured by verify_ruby.py init')
    p.add_argument('--codex', default='codex', help='Codex CLI executable')
    p.add_argument('--model', default='gpt-5.6-luna')
    p.add_argument('--reasoning', default='low')
    p.add_argument('--review-model', default='gpt-5.6-sol')
    p.add_argument('--review-reasoning', default='medium')
    p.add_argument('--jobs', type=int, default=4, help='Concurrent verification workers')
    p.add_argument('--max-iterations', type=int, default=0, help='Implementation attempts per invocation; 0 continues until done')
    p.add_argument('--stall-limit', type=int, default=5, help='Stop after this many attempts without verified progress')
    p.add_argument('--agent-timeout', type=float, default=1800, help='Seconds per implementation or review session')
    p.add_argument('--verify-timeout', type=float, default=900)
    args = p.parse_args(argv)
    if args.jobs < 1 or args.max_iterations < 0 or args.stall_limit < 1 or min(args.agent_timeout, args.verify_timeout) <= 0:
        p.error('Jobs, stall limit, and timeouts must be positive; max iterations must be nonnegative')
    root = args.root.resolve()
    store = Store((args.db or root / 'bindings/ruby/.verification/checks.sqlite3').resolve())
    driver = None
    try:
        recover(store)
        ruby = environment(args.ruby or store.setting('ruby', 'ruby'))[0]['ruby']
        codex = shutil.which(args.codex)
        if not codex: raise DriverError('Codex CLI not found: ' + args.codex)
        options = {key: getattr(args, key) for key in ('model', 'reasoning', 'review_model', 'review_reasoning',
                   'jobs', 'max_iterations', 'stall_limit', 'agent_timeout', 'verify_timeout')}
        options.update(ruby=ruby, codex=codex)
        checks = build(root)
        if args.dry_run:
            print(json.dumps(dict(options=options, database=str(store.path), checks=len(checks),
                assignments=[dict(brief=brief, checks=len(task_ids(checks, brief))) for brief in ORDER]), indent=2))
            return 0
        # One loop per database. Verification subprocesses take their own per-run locks.
        with (store.path.parent / 'driver.lock').open('a+') as lock:
            try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError: raise DriverError('Another driver holds this database lock')
            driver = Driver(root, store, options)
            supplied = argv if argv is not None else sys.argv[1:]
            explicit = {arg.split('=', 1)[0].removeprefix('--').replace('-', '_') for arg in supplied if arg.startswith('--')}
            driver.initialize(args.resume, {key: value for key, value in options.items() if key in explicit})
            return driver.run()
    except BaseException as error:
        if driver and driver.state:
            driver.state['status'] = 'interrupted' if isinstance(error, KeyboardInterrupt) else 'blocked'
            driver.state['last_error'] = str(error)
            driver.save()
            driver.emit('Stopped without completion. Resume with --resume ' + driver.state['loop_id'])
        raise
    finally:
        store.close()


if __name__ == '__main__':
    def interrupted(*_): raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print('Driver interrupted. Worktree, logs, and SQLite history are retained.', file=sys.stderr)
        sys.exit(130)
    except (DriverError, OSError, ValueError) as error:
        print('Driver error: ' + str(error), file=sys.stderr)
        sys.exit(2)
