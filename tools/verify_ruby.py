#!/usr/bin/env python3
"""Verify the Ruby rebuild and retain agent-attributed evidence in local SQLite."""
import argparse
from collections import Counter
import json
from pathlib import Path
import signal
import sys

from ruby_verify.catalog import build, digest
from ruby_verify.runner import Harness, capture, environment, evidence_status, recover, select
from ruby_verify.store import Store


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    p.add_argument('--db', type=Path, help='Default: bindings/ruby/.verification/checks.sqlite3')
    sub = p.add_subparsers(dest='command', required=True)
    init = sub.add_parser('init', help='Create/update catalog and configure Ruby')
    init.add_argument('--ruby', help='Ruby executable (3.3 or newer)')
    run = sub.add_parser('run', help='Capture source, build, execute checks and record all attempts')
    run.add_argument('--agent', required=True, help='Agent identity, for example /root/raw_result')
    for name in ('model', 'reasoning', 'task', 'iteration'):
        run.add_argument('--' + name)
    run.add_argument('--context-file', type=Path, help='JSON object with additional handoff/agent context')
    run.add_argument('--jobs', type=int, default=4)
    run.add_argument('--timeout', type=float, default=20, help='Timeout per Ruby check in seconds')
    run.add_argument('--ruby', help='Override configured Ruby for this run')
    for name in ('status', 'next', 'gate'):
        cmd = sub.add_parser(name)
        cmd.add_argument('--json', action='store_true')
        if name == 'next':
            cmd.add_argument('--limit', type=int, default=10)
    for name in ('run', 'status', 'next', 'gate'):
        cmd = sub.choices[name]
        cmd.add_argument('--brief', help='Exact repository-relative brief path')
        cmd.add_argument('--check', action='append', help='Stable check ID or glob; repeatable; shared gates always included')
    show = sub.add_parser('show', help='Check specification, latest evidence and historical agent context')
    show.add_argument('check_id')
    show.add_argument('--limit', type=int, default=20)
    runs = sub.add_parser('runs', help='Recent runs, counts and attribution as JSON')
    runs.add_argument('--limit', type=int, default=20)
    return p


def rows_for(store, checks, root, ruby):
    source = capture(root)[2]
    _, env_hash = environment(ruby)
    catalog_hash = digest(checks)
    rows = []
    for check in checks:
        last = store.latest(check['id'])
        rows.append(dict(check_id=check['id'], title=check['title'], brief=check['brief'], kind=check['kind'],
                         shared=check.get('shared', False),
                         status=evidence_status(last, check, source, catalog_hash, env_hash),
                         last_status=last['status'] if last else None,
                         message=last['message'] if last else 'No verification attempt',
                         run_id=last['run_id'] if last else None,
                         agent=last['agent'] if last else None,
                         context=json.loads(last['context_json']) if last else None))
    return rows


def main(argv=None):
    args = parser().parse_args(argv)
    root = args.root.resolve()
    path = (args.db or root / 'bindings/ruby/.verification/checks.sqlite3').resolve()
    store = Store(path)
    try:
        recover(store)
        checks = build(root)
        store.sync(checks)
        ruby = getattr(args, 'ruby', None) or store.setting('ruby', 'ruby')
        if args.command == 'init':
            info, _ = environment(ruby)
            store.set_setting('ruby', info['ruby'])
            print(json.dumps(dict(database=str(path), checks=len(checks), kinds=dict(Counter(c['kind'] for c in checks)), environment=info), indent=2))
            return 0
        if args.command == 'run':
            if not args.agent.strip() or args.jobs < 1 or args.timeout <= 0:
                raise ValueError('Nonempty --agent, positive --jobs and --timeout are required')
            extra = json.loads(args.context_file.read_text()) if args.context_file else {}
            if not isinstance(extra, dict):
                raise ValueError('Context file must contain a JSON object')
            context = {name: getattr(args, name) for name in ('agent', 'model', 'reasoning', 'task', 'iteration')}
            context['extra'] = extra
            return Harness(root, store, ruby, jobs=args.jobs, timeout=args.timeout).run(context, args.brief, args.check)
        if args.command == 'show':
            check = next((c for c in checks if c['id'] == args.check_id), None)
            if check is None:
                raise ValueError('Unknown check ID: ' + args.check_id)
            print(json.dumps(dict(check=check, latest=store.latest(args.check_id), history=store.history(args.check_id, args.limit)), indent=2))
            return 0
        if args.command == 'runs':
            rows = [dict(row) for row in store.db.execute('SELECT * FROM runs ORDER BY started_at DESC LIMIT ?', (args.limit,))]
            for row in rows:
                row.pop('source_manifest_json')
                row['counts'] = {r[0]: r[1] for r in store.db.execute('SELECT status,count(*) FROM results WHERE run_id=? GROUP BY status', (row['run_id'],))}
            print(json.dumps(rows, indent=2))
            return 0
        chosen = select(checks, args.brief, args.check)
        selected = {c['id'] for c in chosen}
        rows = [r for r in rows_for(store, checks, root, ruby) if r['check_id'] in selected]
        counts = dict(Counter(r['status'] for r in rows))
        accepted = all(r['status'] == 'pass' for r in rows)
        scope = 'selected checks plus shared gates' if args.brief or args.check else 'complete binding'
        if args.command == 'next':
            rows = [r for r in rows if r['status'] != 'pass']
            rows.sort(key=lambda r: (not r['shared'], r['brief'], r['check_id']))
            rows = rows[:args.limit]
        output = dict(scope=scope, accepted=accepted, counts=counts, checks=rows, database=str(path))
        if args.json:
            print(json.dumps(output, indent=2))
        else:
            print(f'{scope}: ' + ', '.join(f'{status}={count}' for status, count in sorted(counts.items())))
            if args.command == 'next':
                for row in rows:
                    print(f"{row['status']:10} {row['check_id']}\n  {row['brief']}\n  {row['message']}")
            elif args.command == 'gate':
                print('ACCEPTED' if accepted else 'REJECTED: every selected check needs a fresh pass')
            else:
                by_brief = {}
                for row in rows:
                    by_brief.setdefault(row['brief'], Counter())[row['status']] += 1
                for brief, summary in sorted(by_brief.items()):
                    print(brief + ': ' + ', '.join(f'{k}={v}' for k, v in sorted(summary.items())))
        return 0 if args.command != 'gate' or accepted else 1
    finally:
        store.close()


if __name__ == '__main__':
    # Let the run cleanup record interruption and kill workers on SIGTERM too.
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print('Verification interrupted; incomplete evidence cannot pass.', file=sys.stderr)
        sys.exit(130)
    except (OSError, ValueError, RuntimeError) as error:
        print(f'Verification error: {error}', file=sys.stderr)
        sys.exit(2)
