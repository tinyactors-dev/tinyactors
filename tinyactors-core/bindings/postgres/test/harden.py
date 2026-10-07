#!/usr/bin/env python3
"""Production hardening of the tinyactors extension: one live reproduction
per finding of the audits (tools/check_pg_hardening.py, H1-H24).

Runs against the throwaway server test/run.sh starts (PGHOST/PGPORT/PGUSER;
TA_PG_DATA is its data directory, so the server log can be read and a crash
would only hurt the test server). Each test gets a fresh database. After
every test the server must not have restarted (no backend crash).

  python3 test/harden.py [-k substring]
"""
import os
import re
import subprocess
import sys
import time

DB = "tinyactors_harden"
LOG = os.path.join(os.environ.get("TA_PG_DATA", ""), "server.log")
SC = '<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="{name}" initial="{initial}">{body}</scxml>'


class Fail(Exception):
    pass


def psql(sql, db=DB, user=None, timeout=120, check=True, opts=None):
    """Run sql (a script) with psql; returns stdout (tuples only, unaligned)."""
    cmd = ["psql", "-qXtA", "-v", "ON_ERROR_STOP=1", "-d", db]
    if user:
        cmd += ["-U", user]
    env = dict(os.environ, PGOPTIONS="-c client_min_messages=warning " + (opts or ""))
    r = subprocess.run(cmd, input=sql, capture_output=True, text=True, timeout=timeout, env=env)
    if check and r.returncode:
        raise Fail(f"psql failed: {r.stderr.strip()}\n  sql: {sql.strip()[:300]}")
    return r.stdout.strip() if check else (r.returncode, r.stdout.strip(), r.stderr.strip())


def err(sql, **kw):
    """sql must fail; returns the error text."""
    code, out, e = psql(sql, check=False, **kw)
    if code == 0:
        raise Fail(f"expected an error, got success ({out[:200]})\n  sql: {sql.strip()[:300]}")
    return e


def bg(sql, db=DB, user=None, opts=None):
    cmd = ["psql", "-qXtA", "-v", "ON_ERROR_STOP=1", "-d", db] + (["-U", user] if user else [])
    env = dict(os.environ, PGOPTIONS="-c client_min_messages=warning " + (opts or ""))
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    p.stdin.write(sql)
    p.stdin.close()
    return p


def check(cond, what):
    if not cond:
        raise Fail(what)


def eq(got, want, what):
    if got != want:
        raise Fail(f"{what}: got {got!r}, want {want!r}")


def define(name, body, initial="s"):
    src = SC.format(name=name, initial=initial, body=body)
    psql(f"SELECT tinyactors.define('{name}', $scxml${src}$scxml$);")


def spawn(chart, name=None, input_="{}"):
    n = f"'{name}'" if name else "NULL"
    return int(psql(f"SELECT tinyactors.spawn('{chart}', '{input_}', {n});"))


def hwm_sql():
    """The backend's peak resident memory in MB (getrusage: Linux and macOS)."""
    return "SELECT tinyactors._peak_memory() / 1048576;"


def started():
    return psql("SELECT pg_postmaster_start_time();", db="postgres")


def fresh():
    psql(f"DROP DATABASE IF EXISTS {DB} WITH (FORCE);", db="postgres")
    psql(f"CREATE DATABASE {DB};", db="postgres")
    psql("CREATE EXTENSION tinyactors;")


def wait_gone(p, seconds):
    try:
        p.wait(timeout=seconds)
        return True
    except subprocess.TimeoutExpired:
        return False


# ---------------------------------------------------------------- H1
def test_h1_js_is_interruptible():
    loop = '<script>while (true) {}</script>'
    define("loop", f'<state id="s"><transition event="go">{loop}</transition>'
                   f'<transition event="error.execution">{loop}</transition></state>')
    a = spawn("loop")
    t0 = time.time()
    huge = "-c tinyactors.fuel=2000000000"  # only the interrupt can stop it
    e = err(f"SET statement_timeout = '1s'; SELECT tinyactors.send({a}, 'go');", timeout=60, opts=huge)
    check("statement timeout" in e, f"statement_timeout must cancel it, got: {e}")
    check(time.time() - t0 < 6, f"statement_timeout took {time.time() - t0:.1f} s")
    # pg_cancel_backend
    p = bg(f"SELECT tinyactors.send({a}, 'go');", opts=huge)
    time.sleep(1.0)
    psql(f"SELECT pg_cancel_backend(pid) FROM pg_stat_activity WHERE datname = '{DB}' AND query LIKE '%tinyactors.send%' AND pid <> pg_backend_pid();")
    check(wait_gone(p, 6), "pg_cancel_backend did not stop the loop within 6 s")
    check("canceling statement" in p.stderr.read(), "the send should end with a cancel error")
    # JS work is charged as fuel: a long (finite) script fails on fuel, quickly
    define("busy", '<state id="s"><transition event="go"><script>for (let i = 0; i &lt; 2e9; i++) {}</script></transition></state>')
    b = spawn("busy")
    t0 = time.time()
    e = err(f"SELECT tinyactors.send({b}, 'go');", timeout=120)
    check("fuel" in e, f"a long script must fail on fuel, got: {e}")
    check(time.time() - t0 < 20, f"fuel limit took {time.time() - t0:.1f} s")
    # ordinary scripts still run
    define("ok", '<datamodel><data id="n" expr="0"/></datamodel><state id="s"><transition event="go"><script>for (let i = 0; i &lt; 100000; i++) n++;</script></transition></state>')
    c = spawn("ok")
    psql(f"SELECT tinyactors.send({c}, 'go');")
    eq(psql(f"SELECT datamodel->>'n' FROM tinyactors.actors WHERE id = {c};"), "100000", "ordinary work")


# ---------------------------------------------------------------- H2
def test_h2_reentry_is_an_error_not_a_crash():
    define("p1", '<state id="s"><transition event="t"><script>throw new Error("boom")</script><raise event="x"/></transition>'
                 '<transition event="x" target="loop"/></state><state id="loop"><transition target="loop2"/></state><state id="loop2"><transition target="loop"/></state>')
    define("emit", '<state id="s"><transition event="go"><send type="outbox" target="fwd" event="x"/></transition></state>')
    spawn("p1", "p1")
    e1 = spawn("emit")
    psql("""
CREATE FUNCTION public.reenter() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  BEGIN
    PERFORM tinyactors.send('p1', 't');
  EXCEPTION WHEN others THEN
    RAISE NOTICE 'caught: %', SQLERRM;
  END;
  RETURN NULL;
END $$;
CREATE TRIGGER reenter AFTER INSERT ON tinyactors.outbox FOR EACH ROW EXECUTE FUNCTION public.reenter();""")
    psql(f"SELECT tinyactors.send({e1}, 'go');")  # the trigger's call is refused; caught; the send succeeds
    eq(psql(f"SELECT count(*) FROM tinyactors.outbox WHERE actor = {e1};"), "1", "the effect is stored")
    psql("DROP TRIGGER reenter ON tinyactors.outbox;")
    psql("""
CREATE FUNCTION public.reenter2() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN PERFORM tinyactors.send('p1', 't'); RETURN NULL; END $$;
CREATE TRIGGER reenter2 AFTER INSERT ON tinyactors.outbox FOR EACH ROW EXECUTE FUNCTION public.reenter2();""")
    e = err(f"SELECT tinyactors.send({e1}, 'go');")
    check("while an actor is being processed" in e, f"a clean re-entry error, got: {e}")


# ---------------------------------------------------------------- H3
def test_h3_one_failing_actor_does_not_stop_run():
    psql("SET tinyactors.now = '2030-01-01 00:00:00+00';")
    define("bad", '<state id="s"><onentry><send event="tick" delay="1s"/></onentry><transition event="tick" target="b"/></state>'
                  '<state id="b"><transition target="c"/></state><state id="c"><transition target="b"/></state>')
    define("good", '<state id="s"><onentry><send event="tick" delay="1s"/></onentry><transition event="tick" target="fired"/></state><state id="fired"/>')
    opts = "-c tinyactors.now=2030-01-01T00:00:00Z"
    bad = int(psql("SELECT tinyactors.spawn('bad');", opts=opts))
    goods = [int(psql("SELECT tinyactors.spawn('good');", opts=opts)) for _ in range(3)]
    later = "-c tinyactors.now=2030-01-01T00:00:05Z -c tinyactors.fuel=100000"
    psql("SELECT tinyactors.run();", opts=later)  # must not fail
    for g in goods:
        eq(psql(f"SELECT configuration FROM tinyactors.actors WHERE id = {g};"), "{fired}", "a healthy actor's timer fired")
    row = psql(f"SELECT failures, last_error LIKE '%fuel%', retry_at > '2030-01-01 00:00:05+00', status FROM tinyactors.actors WHERE id = {bad};")
    eq(row, "1|t|t|active", "the failure is recorded on the failing actor (failures, last_error, retry_at)")
    # backoff: run() leaves it alone until retry_at
    psql("SELECT tinyactors.run();", opts=later)
    eq(psql(f"SELECT failures FROM tinyactors.actors WHERE id = {bad};"), "1", "not retried before retry_at")
    # a failing cascade target does not abort the sender
    define("loopy", '<state id="s"><transition event="x" target="b"/></state><state id="b"><transition target="c"/></state><state id="c"><transition target="b"/></state>')
    define("sender", '<datamodel><data id="to" expr="null"/></datamodel><state id="s"><transition event="go" target="sent"><send targetexpr="to" event="x"/></transition></state><state id="sent"/>')
    lo = spawn("loopy")
    se = spawn("sender", input_=f'{{"to": "#_scxml_{lo}"}}')
    psql(f"SELECT tinyactors.send({se}, 'go');", opts="-c tinyactors.fuel=100000")
    eq(psql(f"SELECT configuration FROM tinyactors.actors WHERE id = {se};"), "{sent}", "the sender committed")
    eq(psql(f"SELECT failures, (SELECT count(*) FROM tinyactors.inbox WHERE actor = {lo}) FROM tinyactors.actors WHERE id = {lo};"), "1|1",
       "the target's failure is recorded and its event kept")
    # the addressed actor's own failure is the caller's error
    err(f"SELECT tinyactors.send({lo}, 'x');", opts="-c tinyactors.fuel=100000")


# ---------------------------------------------------------------- H4
def test_h4_deep_nesting_is_refused():
    define("sink", '<datamodel><data id="n" expr="0"/><data id="deep" expr="null"/></datamodel><state id="s">'
                   '<transition event="x"><assign location="n" expr="n + 1"/></transition>'
                   '<transition event="build"><script>var v = []; for (var i = 0; i &lt; 5000; i++) v = [v]; deep = v;</script></transition></state>')
    a = spawn("sink", "sink")
    deep = "[" * 4000 + "]" * 4000
    e = err(f"SELECT tinyactors.send({a}, 'x', '{deep}');")
    check("nested" in e, f"deep data must be refused, got: {e}")
    eq(psql(f"""SELECT tinyactors.deliver(jsonb_build_object('target', 'tinyactors://' || tinyactors.system_id() || '/@sink',
             'name', 'x', 'data', '{deep}'::jsonb));"""), "malformed", "deliver(envelope) refuses it")
    psql(f"SELECT tinyactors.send({a}, 'x', '[[1]]');")
    # a chart that builds a value too deep to store: a clean error, the actor stays usable
    e = err(f"SELECT tinyactors.send({a}, 'build');")
    check("variable 'deep' cannot be stored" in e, f"the error names the variable, got: {e}")
    psql(f"SELECT tinyactors.send({a}, 'x');")
    eq(psql(f"SELECT datamodel->>'n' FROM tinyactors.actors WHERE id = {a};"), "2", "the actor still works")


# ---------------------------------------------------------------- H5
def test_h5_image_cache_is_never_stale():
    a_src = SC.format(name="stale", initial="old_a", body='<datamodel><data id="v" expr="\'old\'"/></datamodel><state id="old_a"/>')
    b_src = SC.format(name="stale", initial="new_b", body='<datamodel><data id="v" expr="\'new\'"/></datamodel><state id="new_b"/>')
    out = psql(f"""BEGIN; SELECT tinyactors.define('stale', $s${a_src}$s$); SELECT tinyactors.spawn('stale'); ROLLBACK;
SELECT tinyactors.define('stale', $s${b_src}$s$) AS v;
SELECT tinyactors.spawn('stale') AS id \\gset
SELECT configuration::text || ' ' || (datamodel->>'v') FROM tinyactors.actors WHERE id = :id;""")
    check(out.splitlines()[-1] == "{new_b} new", f"the same backend runs the new chart, got: {out}")
    a = psql("SELECT id FROM tinyactors.actors WHERE chart = 'stale' ORDER BY id DESC LIMIT 1;")
    psql(f"SELECT tinyactors.send({a}, 'anything');")  # another backend loads it
    # DROP/CREATE EXTENSION in one backend: the cache follows
    out = psql(f"""SELECT tinyactors.spawn('stale') > 0; DROP EXTENSION tinyactors CASCADE; CREATE EXTENSION tinyactors;
SELECT tinyactors.define('stale', $s${a_src}$s$);
SELECT tinyactors.spawn('stale') AS id \\gset
SELECT configuration::text FROM tinyactors.actors WHERE id = :id;""")
    eq(out.splitlines()[-1], "{old_a}", "after DROP/CREATE EXTENSION the current image is used")


# ---------------------------------------------------------------- H6
def test_h6_backlog_is_processed_in_batches():
    define("seq", '<datamodel><data id="n" expr="0"/><data id="last" expr="0"/><data id="bad" expr="0"/></datamodel><state id="s">'
                  '<transition event="e"><if cond="_event.data.i != last + 1"><assign location="bad" expr="bad + 1"/></if>'
                  '<assign location="last" expr="_event.data.i"/><assign location="n" expr="n + 1"/></transition></state>')
    a = spawn("seq")
    psql(f"INSERT INTO tinyactors.inbox (actor, event, data) SELECT {a}, 'e', jsonb_build_object('i', g, 'pad', repeat('x', 2000)) FROM generate_series(1, 100000) g;")
    out = psql(f"SELECT tinyactors.process({a}); " + hwm_sql())
    mb = int(out.splitlines()[-1])
    check(mb < 200, f"one processing of a 100k backlog peaked at {mb} MB")
    # send() to a backlogged actor queues behind the backlog
    psql(f"SELECT tinyactors.deliver({a}, 'e', '{{\"i\": 100001}}');")
    for _ in range(300):
        left = int(psql(f"SELECT count(*) FROM tinyactors.inbox WHERE actor = {a};"))
        if not left:
            break
        psql("SELECT tinyactors.run();")
    eq(psql(f"SELECT datamodel->>'n', datamodel->>'bad' FROM tinyactors.actors WHERE id = {a};"), "100001|0", "every event, in order")


# ---------------------------------------------------------------- H7
def test_h7_strings_jsonb_rejects_are_stored():
    define("nul", r'<datamodel><data id="s" expr="null"/><data id="t" expr="null"/></datamodel><state id="s0">'
                  r'<transition event="z"><assign location="s" expr="&apos;a\u0000b&apos;"/><assign location="t" expr="&apos;x\ud800y&apos;"/>'
                  r'<send type="outbox" target="t" event="e"><param name="v" expr="s + t"/></send></transition></state>', initial="s0")
    a = spawn("nul")
    psql(f"SELECT tinyactors.send({a}, 'z');")
    eq(psql(f"SELECT datamodel->>'s', datamodel->>'t' FROM tinyactors.actors WHERE id = {a};"), "a\ufffdb|x\ufffdy", "stored as U+FFFD")
    eq(psql(f"SELECT data->>'v' FROM tinyactors.outbox WHERE actor = {a};"), "a\ufffdbx\ufffdy", "the effect too")


# ---------------------------------------------------------------- H8
def test_h8_js_memory_is_limited():
    t0 = started()
    define("hog", '<datamodel><data id="a" expr="[]"/><data id="err" expr="null"/></datamodel><state id="s">'
                  '<transition event="hog"><script>for (let i = 0; i &lt; 12000; i++) a.push(new Array(10000).fill(0));</script></transition>'
                  '<transition event="error.execution"><assign location="a" expr="[]"/><assign location="err" expr="_event.name"/></transition></state>')
    a = spawn("hog")
    code, out, e = psql(f"SELECT tinyactors.send({a}, 'hog'); " + hwm_sql(), check=False, opts="-c tinyactors.js_memory_limit=128MB", timeout=120)
    eq(started(), t0, "the server must not restart")
    if code == 0:
        mb = int(out.splitlines()[-1])
        check(mb < 600, f"backend peaked at {mb} MB with a 128 MB JS limit")
        eq(psql(f"SELECT datamodel->>'err' FROM tinyactors.actors WHERE id = {a};"), "error.execution", "the hog got error.execution")
    else:
        check("memory" in e, f"a memory error, got: {e}")
    # stored state is capped
    define("big", '<datamodel><data id="s" expr="null"/></datamodel><state id="s0"><transition event="go"><assign location="s" expr="\'x\'.repeat(3000000)"/></transition></state>', initial="s0")
    b = spawn("big")
    e = err(f"SELECT tinyactors.send({b}, 'go');", opts="-c tinyactors.max_state_bytes=1MB")
    check("too large" in e, f"state over max_state_bytes must be refused, got: {e}")


# ---------------------------------------------------------------- H9
def test_h9_system_growth_is_bounded():
    t0 = started()
    src = SC.format(name="tinyactors:ib", initial="s",
                    body='<state id="s"><invoke srcexpr="\'tinyactors:ib\'"/><invoke srcexpr="\'tinyactors:ib\'"/></state>')
    code, out, e = psql(f"SELECT tinyactors.define('ib', $s${src}$s$);", check=False)
    if code:
        return  # a chart that invokes itself does not even compile: bounded
    t = time.time()
    code, out, e = psql("SELECT tinyactors.spawn('ib'); " + hwm_sql(), check=False, timeout=120)
    eq(started(), t0, "the server must not restart")
    check(code != 0, "endless invocation must fail")
    check(("fuel" in e or "too large" in e) and "invalid memory alloc" not in e, f"a fuel/size error, got: {e}")
    check(time.time() - t < 30, "it must stop quickly")


# ---------------------------------------------------------------- H10
def test_h10_privileges():
    for r in ["ta_app", "ta_worker", "ta_nobody"]:
        psql(f"DROP ROLE IF EXISTS {r}; CREATE ROLE {r} LOGIN;", db="postgres")
    psql("SELECT tinyactors.grant('ta_app', 'app'); SELECT tinyactors.grant('ta_worker', 'worker');")
    eq(psql("""SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
               WHERE n.nspname = 'tinyactors' AND has_function_privilege('ta_nobody', p.oid, 'EXECUTE');"""), "0",
       "nothing is executable by PUBLIC")
    define("c", '<datamodel><data id="n" expr="0"/></datamodel><state id="s"><transition event="inc"><assign location="n" expr="n + 1"/>'
                '<send type="outbox" target="x" event="y"/></transition></state>')
    a = int(psql("SELECT tinyactors.spawn('c');", user="ta_app"))
    psql(f"SELECT tinyactors.send({a}, 'inc');", user="ta_app")
    eq(psql(f"SELECT datamodel->>'n' FROM tinyactors.actors WHERE id = {a};", user="ta_app"), "1", "the app role reads actors")
    check("permission denied" in err("SELECT tinyactors.system_id(); SET tinyactors.now = 'infinity';", user="ta_app"),
          "tinyactors.now is superuser-only")
    eq(psql("SET tinyactors.now = 'infinity'; SELECT tinyactors.now() < 'infinity';", user="ta_app", check=False)[1].splitlines()[-1], "t",
       "set before the extension loaded, it is discarded")
    for sql in ["SELECT tinyactors.define('x', '<scxml/>');", "SELECT * FROM tinyactors.claim();",
                "SELECT tinyactors.hello('web');", "SELECT * FROM tinyactors.claim_for('web');",
                "DELETE FROM tinyactors.actors;", "UPDATE tinyactors.outbox SET error = 'x';"]:
        check("permission denied" in err(sql, user="ta_app"), f"app must not: {sql}")
    check("permission denied" in err(f"SELECT tinyactors.send({a}, 'inc');", user="ta_nobody"), "no grant, no send")
    rows = psql("SELECT count(*) FROM tinyactors.claim(10, 'w');", user="ta_worker")
    eq(rows, "1", "the worker claims effects")
    psql("SELECT tinyactors.ack(id) FROM tinyactors.outbox;", user="ta_worker")
    check("permission denied" in err(f"SELECT tinyactors.send({a}, 'inc');", user="ta_worker"), "a worker does not send")


# ---------------------------------------------------------------- H11
def test_h11_peers_are_authenticated():
    for r in ["ta_link_web", "ta_link_other"]:
        psql(f"DROP ROLE IF EXISTS {r}; CREATE ROLE {r} LOGIN;", db="postgres")
    psql("""SELECT tinyactors.grant('ta_link_web', 'link'); SELECT tinyactors.grant('ta_link_other', 'link');
SELECT tinyactors.grant_peer('web', 'ta_link_web'); SELECT tinyactors.grant_peer('other', 'ta_link_other');""")
    define("snd", '<state id="s"><transition event="go"><send target="tinyactors://web/#_scxml_99" event="secret"><param name="pan" expr="4111"/></send></transition></state>')
    a = spawn("snd")
    psql(f"SELECT tinyactors.send({a}, 'go');")
    psql("SELECT tinyactors.hello('web', '{}');", user="ta_link_web")
    check("not allowed" in err("SELECT tinyactors.hello('web', '{}');", user="ta_link_other"), "hello as another peer")
    check("not allowed" in err("SELECT * FROM tinyactors.claim_for('web');", user="ta_link_other"), "claim another peer's envelopes")
    psql("SELECT tinyactors.hello('other', ARRAY['web']);", user="ta_link_other")
    eq(psql("SELECT peer FROM tinyactors.outbox WHERE system = 'web';"), "web", "announcing someone else's system steals nothing")
    eq(psql("SELECT reach FROM tinyactors.peers WHERE id = 'other';"), "{}", "a granted peer's system is not accepted as reach")
    eq(psql("SELECT count(*) FROM tinyactors.claim_for('web');", user="ta_link_web"), "1", "the right peer claims")
    oid = psql("SELECT id FROM tinyactors.outbox WHERE system = 'web';")
    check("not allowed" in err(f"SELECT tinyactors.ack({oid});", user="ta_link_other"), "ack another peer's envelope")
    psql(f"SELECT tinyactors.ack({oid});", user="ta_link_web")


# ---------------------------------------------------------------- H12
def test_h12_multi_statement_transactions_without_hidden_locks():
    define("fw", '<datamodel><data id="to" expr="null"/></datamodel><state id="s"><transition event="e"><if cond="to"><send targetexpr="to" event="e"/></if></transition></state>')
    b = spawn("fw")
    a = spawn("fw", input_=f'{{"to": "#_scxml_{b}"}}')
    c = spawn("fw")
    opts = "-c tinyactors.cascade=0"
    t1 = bg(f"BEGIN; SELECT tinyactors.send({a}, 'e'); SELECT pg_sleep(1.5); SELECT tinyactors.send({c}, 'e'); COMMIT;", opts=opts)
    time.sleep(0.5)
    t2 = bg(f"BEGIN; SELECT tinyactors.send({c}, 'e'); SELECT tinyactors.send({b}, 'e'); SELECT pg_sleep(1); COMMIT;", opts=opts)
    check(wait_gone(t1, 20) and wait_gone(t2, 20), "both transactions end")
    e1, e2 = t1.stderr.read(), t2.stderr.read()
    check(t1.returncode == 0 and t2.returncode == 0, f"no deadlock with tinyactors.cascade = 0: {e1} {e2}")


# ---------------------------------------------------------------- H13
def test_h13_reused_sendids_keep_every_effect():
    define("order", '<datamodel><data id="total" expr="0"/></datamodel><state id="cart">'
                    '<transition event="checkout" target="paying"><send type="outbox" target="payments" event="charge" id="charge"/></transition></state>'
                    '<state id="paying"><transition event="error.communication" target="cart"/></state>', initial="cart")
    o = spawn("order")
    psql(f"SELECT tinyactors.send({o}, 'checkout');")
    rid = psql(f"SELECT id FROM tinyactors.outbox WHERE actor = {o};")
    psql(f"SELECT tinyactors.fail({rid}, 'card declined');")
    psql(f"SELECT tinyactors.send({o}, 'checkout');")
    eq(psql(f"SELECT count(*), count(DISTINCT idempotency_key) FROM tinyactors.outbox WHERE actor = {o} AND sendid = 'charge';"), "2|2",
       "the retried charge is a new effect")
    # keys are stable when a rolled-back event is retried
    k1 = psql(f"BEGIN; SELECT tinyactors.send({o}, 'error.communication'); SELECT tinyactors.send({o}, 'checkout'); "
              f"SELECT idempotency_key FROM tinyactors.outbox WHERE actor = {o} ORDER BY id DESC LIMIT 1; ROLLBACK;").splitlines()[-1]
    k2 = psql(f"SELECT tinyactors.send({o}, 'error.communication'); SELECT tinyactors.send({o}, 'checkout'); "
              f"SELECT idempotency_key FROM tinyactors.outbox WHERE actor = {o} ORDER BY id DESC LIMIT 1;").splitlines()[-1]
    eq(k1, k2, "the same event retried after a rollback gives the same key")


# ---------------------------------------------------------------- H14
def test_h14_auto_sendids_are_deterministic():
    define("echo", '<state id="s"><transition event="go"><send type="outbox" target="t" event="x"/></transition></state>')
    e = spawn("echo")
    s1 = psql(f"BEGIN; SELECT tinyactors.send({e}, 'go'); SELECT sendid FROM tinyactors.outbox WHERE actor = {e}; ROLLBACK;").splitlines()[-1]
    s2 = psql(f"SELECT tinyactors.send({e}, 'go'); SELECT sendid FROM tinyactors.outbox WHERE actor = {e};").splitlines()[-1]
    eq(s1, s2, "an automatic sendid does not depend on the clock")
    check(not re.search(r"[0-9a-f]{12}", s1), f"no timestamp in {s1}")
    for _ in range(30):
        psql(f"BEGIN; SELECT tinyactors.send({e}, 'go'); SELECT tinyactors.send({e}, 'go'); COMMIT;")
    eq(psql(f"SELECT count(*) FROM tinyactors.outbox WHERE actor = {e};"), "61", "no effect lost")


# ---------------------------------------------------------------- H15
def test_h15_outbox_and_seen_are_pruned():
    idx = psql("SELECT indexdef FROM pg_indexes WHERE indexname = 'outbox_pending';")
    check("error IS NULL" in idx, f"claim()'s index skips failed rows: {idx}")
    define("fx", '<state id="s"><transition event="go"><send type="outbox" target="t" event="x"/></transition></state>')
    a = spawn("fx", "fx")
    for _ in range(3):
        psql(f"SELECT tinyactors.send({a}, 'go');")
    psql("SELECT tinyactors.ack(id) FROM tinyactors.outbox;")
    psql("UPDATE tinyactors.outbox SET delivered_at = now() - interval '30 days';")
    psql("""SELECT tinyactors.deliver(jsonb_build_object('target', 'tinyactors://' || tinyactors.system_id() || '/@fx', 'name', 'go',
            'origin', 'tinyactors://o/#_scxml_1', 'msgid', 'm' || g)) FROM generate_series(1, 5) g;""")
    psql("UPDATE tinyactors.seen SET at = now() - interval '30 days';")
    psql("SELECT tinyactors.run();", opts="-c tinyactors.outbox_retention=7d")
    eq(psql("SELECT count(*) FROM tinyactors.outbox WHERE delivered_at < now() - interval '7 days';"), "0", "old delivered rows are pruned by run()")
    eq(psql("SELECT count(*) FROM tinyactors.seen;"), "0", "the dedup window is pruned by run()")


# ---------------------------------------------------------------- H16
def test_h16_run_does_not_starve():
    define("sink", '<datamodel><data id="n" expr="0"/></datamodel><state id="s"><transition event="e"><assign location="n" expr="n + 1"/></transition></state>')
    x = spawn("sink")
    y = spawn("sink")
    psql(f"INSERT INTO tinyactors.inbox (actor, event) SELECT {x}, 'e' FROM generate_series(1, 5000);")
    psql(f"INSERT INTO tinyactors.inbox (actor, event) VALUES ({y}, 'e');")
    hold = bg(f"BEGIN; SELECT 1 FROM tinyactors.actors WHERE id = {x} FOR UPDATE; SELECT pg_sleep(5); COMMIT;")
    time.sleep(0.5)
    n = int(psql("SELECT tinyactors.run(10);"))
    check(n >= 1, "run() processed someone")
    eq(psql(f"SELECT datamodel->>'n' FROM tinyactors.actors WHERE id = {y};"), "1", "the other queued actor is processed")
    hold.kill()


# ---------------------------------------------------------------- H17
def test_h17_notify_backs_off():
    src = psql("SELECT prosrc FROM pg_proc WHERE proname = 'notify_outbox';")
    check("pg_notification_queue_usage" in src and "tinyactors.notify" in src, "the trigger backs off when the queue fills")
    define("fx", '<state id="s"><transition event="go"><send type="outbox" target="t" event="x"/></transition></state>')
    a = spawn("fx")
    lis = bg("LISTEN tinyactors_outbox; SELECT pg_sleep(2);")
    time.sleep(0.5)
    psql(f"SELECT tinyactors.send({a}, 'go');", opts="-c tinyactors.notify=off")
    check(wait_gone(lis, 10), "listener ends")
    check("Asynchronous notification" not in lis.stdout.read(), "tinyactors.notify = off sends no notification")


# ---------------------------------------------------------------- H18
def test_h18_log_flood_is_capped():
    define("logger", '<state id="s"><transition event="go"><foreach array="Array.from({length: 5000}, (_, i) =&gt; i)" item="i">'
                     '<log label="flood" expr="\'line \' + i"/></foreach></transition></state>')
    a = spawn("logger")
    before = os.path.getsize(LOG)
    psql(f"SELECT tinyactors.send({a}, 'go');", opts="-c tinyactors.log=log -c tinyactors.log_limit=50")
    time.sleep(0.5)
    with open(LOG, encoding="utf-8", errors="replace") as f:
        f.seek(before)
        new = f.read()
    n = new.count(": flood: line ")
    check(n <= 50, f"{n} log lines (limit 50)")
    check("more log lines" in new, "a note says how many were suppressed")


# ---------------------------------------------------------------- H19
def test_h19_datamodel_column_is_optional():
    define("bigdm", '<datamodel><data id="items" expr="[]"/></datamodel><state id="s"><transition event="add"><script>items.push(items.length)</script></transition></state>')
    a = spawn("bigdm")
    opts = "-c tinyactors.store_datamodel=off"
    psql(f"SELECT tinyactors.send({a}, 'add'); SELECT tinyactors.send({a}, 'add');", opts=opts)
    eq(psql(f"SELECT datamodel IS NULL, events FROM tinyactors.actors WHERE id = {a};"), "t|2", "no jsonb copy written")
    psql(f"SELECT tinyactors.send({a}, 'add');")
    eq(psql(f"SELECT jsonb_array_length(datamodel->'items') FROM tinyactors.actors WHERE id = {a};"), "3", "the state was kept")


# ---------------------------------------------------------------- H20
def test_h20_compile_is_bounded():
    big = "<state id='a'>" * 60000 + "</state>" * 60000
    src = SC.format(name="huge", initial="a", body=big)
    t = time.time()
    e = err(f"SET statement_timeout = '2s'; SELECT tinyactors.define('huge', $s${src}$s$);", timeout=60)
    check("too large" in e or "statement timeout" in e, f"refused, got: {e[:200]}")
    check(time.time() - t < 5, f"took {time.time() - t:.1f} s")

    # past the size cap the compiler is linear: the audit's documents (2.7 MB
    # nested 60,000 deep: 39 s; 80,000 siblings: 8.2 s) take milliseconds
    lift = "-c tinyactors.max_chart_bytes=1GB"
    t = time.time()
    e = err(f"SELECT tinyactors.compile($s${src}$s$, 'huge');", opts=lift, timeout=60)
    check("state nesting too deep" in e, f"rejected for its nesting, got: {e[:200]}")
    check(time.time() - t < 3, f"nested: took {time.time() - t:.1f} s")
    wide = "".join(f'<state id="s{i}"><transition event="e{i}" target="s{(i + 1) % 80000}"/></state>' for i in range(80000))
    src = SC.format(name="wide", initial="s0", body=wide)
    t = time.time()
    psql(f"SELECT length(tinyactors.compile($s${src}$s$, 'wide'));", opts=lift, timeout=60)
    check(time.time() - t < 5, f"siblings: took {time.time() - t:.1f} s")

    # and cancellable: statement_timeout stops a compile that is long even so
    def compile_states(n, timeout_ms=0):
        doc = SC.format(name="many", initial="s", body="<state id=\"s\"/>' || repeat('<state/>', %d) || '" % n)
        t = time.time()
        r = psql(f"SET statement_timeout = {timeout_ms}; SELECT length(tinyactors.compile('{doc}', 'many'));",
                 opts=lift, check=False, timeout=120)
        return time.time() - t, r
    n = 100000
    while True:
        full, r = compile_states(n)
        check(r[0] == 0, f"{n} states compile: {r[2][:200]}")
        if full > 1.0 or n >= 3200000:
            break
        n *= 2
    t, r = compile_states(n, timeout_ms=int(full * 1000 / 5))
    check(r[0] != 0 and "statement timeout" in r[2], f"statement_timeout must cancel it, got: {r[2][:200]}")
    check(t < full * 0.6, f"cancelled after {t:.2f} s of a {full:.2f} s compile")


# ---------------------------------------------------------------- H21
def test_h21_state_ids_are_escaped():
    define("weird", '<state id="x&quot;y"><transition event="go" target="b\\c"/></state><state id="b\\c"/>', initial="x&quot;y")
    a = spawn("weird")
    eq(psql(f"SELECT configuration[1] FROM tinyactors.actors WHERE id = {a};"), 'x"y', "a quote in a state id")
    psql(f"SELECT tinyactors.send({a}, 'go');")
    eq(psql(f"SELECT configuration[1] FROM tinyactors.actors WHERE id = {a};"), "b\\c", "a backslash in a state id")


# ---------------------------------------------------------------- H22
def test_h22_huge_delays_do_not_wrap():
    define("far", '<state id="s"><onentry><send event="ring" delay="300000000000s"/></onentry><transition event="ring" target="rang"/></state><state id="rang"/>')
    a = spawn("far")
    eq(psql(f"SELECT next_due_at IS NULL OR next_due_at > '5000-01-01' FROM tinyactors.actors WHERE id = {a};"), "t", "never earlier than asked")
    psql("SELECT tinyactors.run();", opts="-c tinyactors.now=2300-01-01T00:00:00Z")
    eq(psql(f"SELECT configuration FROM tinyactors.actors WHERE id = {a};"), "{s}", "it has not fired")


def test_h23_host_router_storage():
    define("ledger", '<datamodel><data id="n" expr="0"/><data id="ty" expr="null"/></datamodel><state id="s">'
           '<transition event="add"><assign location="n" expr="n + 1"/>'
           '<assign location="ty" expr="_event.origintype"/>'
           '<send targetexpr="_event.origin" event="reply" id="reused"/></transition></state>')
    a = spawn("ledger", "ledger")
    me = psql("SELECT tinyactors.system_id();")
    psql("SELECT tinyactors.hello('web', ARRAY['tab']);")

    def deliver(origin="tab", key="m1", hops=0, target="@ledger", peer="web"):
        return ("SELECT tinyactors.deliver(jsonb_build_object("
                f"'target','tinyactors://{me}/{target}','name','add',"
                f"'origin','tinyactors://{origin}/#_scxml_7','msgid','{key}','hops',{hops},'origintype','urn:test'),'{peer}');")

    eq(psql(deliver(origin=me)), "denied", "a trusted store link cannot forge our origin")
    eq(psql(deliver(hops=9)), "hops", "hop limit enforced before dedup")
    eq(psql("SELECT count(*) FROM tinyactors.seen;"), "0", "refusals do not poison dedup")
    eq(psql(deliver(target="@missing")), "unknown-target", "missing target is not remembered")
    eq(psql(deliver(hops=8)), "ok", "same msgid succeeds at the hop boundary")
    eq(psql(deliver()), "duplicate", "durable dedup survives a fresh backend")
    eq(psql("SELECT peer, envelope->>'hops' FROM tinyactors.outbox;"), "web|0", "reply is routed by hello at zero hops")
    eq(psql("BEGIN; " + deliver(key="retry") + " ROLLBACK;"), "ok", "delivery can roll back")
    eq(psql(deliver(key="retry")), "ok", "rolled-back dedup is not retained")
    eq(psql(f"SELECT datamodel->>'n' FROM tinyactors.actors WHERE id={a};"), "2", "exactly two deliveries")
    eq(psql(f"SELECT datamodel->>'ty' FROM tinyactors.actors WHERE id={a};"), "urn:test", "origintype survives the inbox")
    eq(psql("SELECT count(DISTINCT envelope->>'msgid') FROM tinyactors.outbox;"), "2", "reused sendid is not a msgid")
    psql("SELECT tinyactors.hello('web', '{}');")
    eq(psql(deliver(key="gone")), "denied", "withdrawn reach no longer authorizes origin")
    eq(psql("SELECT count(*) FROM tinyactors.seen WHERE key='gone';"), "0", "denied origin is not remembered")
    psql("SELECT tinyactors.hello('tab');")
    eq(psql("SELECT bool_and(peer='tab') FROM tinyactors.outbox;"), "t", "direct peer takes precedence over relay")


# ---------------------------------------------------------------- H24
def test_h24_cost_per_event():
    """What an event costs in SQL: statements per send() (pg_stat_statements,
    nested statements of one backend), and none of them planned per event —
    every statement on the processing path is a kept plan, and a send that
    stays in this database does not read the store links' peers."""
    define("session", '<datamodel><data id="ids" expr="[]"/><data id="touches" expr="0"/></datamodel>'
           '<state id="s"><onentry><send event="timeout" delay="3600s" id="timeout"/></onentry>'
           '<transition event="touch" target="s"><cancel sendid="timeout"/>'
           '<if cond="ids.indexOf(_event.data.id) &lt; 0"><script>ids.push(_event.data.id)</script></if>'
           '<assign location="touches" expr="touches + 1"/></transition></state>')
    define("pinger", '<datamodel><data id="n" expr="0"/></datamodel><state id="s">'
           '<transition event="go"><send targetexpr="_event.data.to" event="ping"/></transition>'
           '<transition event="ping"><assign location="n" expr="n + 1"/></transition></state>')
    s, a, b = spawn("session"), spawn("pinger"), spawn("pinger")
    psql("CREATE EXTENSION IF NOT EXISTS pg_stat_statements;")
    # statements per send(): send() itself 2 (exists, the row it returns); each actor
    # processed 3 (lock, inbox, update); a send to an actor 3 (active?, inbox row, its
    # FK check); to another system 3 (peers, outbox row, its NOTIFY)
    cases = {
        "touch": (lambda g: f"tinyactors.send({s}, 'touch', jsonb_build_object('id', {g % 7}))", 5),
        "plain send": (lambda g: f"tinyactors.send({a}, 'go', jsonb_build_object('to', '#_scxml_{b}'))", 11),
        "send to another system": (lambda g: f"tinyactors.send({a}, 'go', jsonb_build_object('to', "
                                   f"'tinyactors://elsewhere/#_scxml_{g + 1}'))", 8),
    }
    n = 50
    for what, (call, want) in cases.items():
        # one backend, every event a transaction of its own (as from a client); the
        # first 20 let the kept plans settle on their generic plan
        out = psql("SET pg_stat_statements.track = 'all';\n"
                   "SET pg_stat_statements.track_planning = on;\n"
                   + "".join(f"SELECT 1 FROM {call(g)};\n" for g in range(20))
                   + "SELECT pg_stat_statements_reset() IS NOT NULL;\n"
                   + "".join(f"SELECT 1 FROM {call(g)};\n" for g in range(20, 20 + n))
                   + r"""SELECT coalesce(sum(calls), 0) || '|' || coalesce(sum(plans), 0) || '|' ||
       coalesce(string_agg(calls || ' ' || plans || ' ' || left(regexp_replace(query, '\s+', ' ', 'g'), 90), E'\n'), '')
  FROM pg_stat_statements WHERE NOT toplevel AND dbid = (SELECT oid FROM pg_database WHERE datname = current_database());""")
        calls, plans, detail = out.split("\n", 21 + n)[21 + n].split("|", 2)  # after the sends
        per = int(calls) / n
        print(f"     {what}: {per:g} statements per event, {int(plans)} planned")
        check(per == want and int(plans) == 0,
              f"{what}: {per:g} statements per event (want {want}), {plans} planned in {n} events "
              f"(want 0); update the numbers only for a reason:\n{detail}")
    eq(psql(f"SELECT datamodel->>'n' FROM tinyactors.actors WHERE id = {b};"), str(20 + n), "the pings arrived")
    eq(psql("SELECT count(*) FROM tinyactors.outbox WHERE system = 'elsewhere';"), str(20 + n),
       "the envelopes for the other system are in the outbox")


def main(argv):
    only = argv[argv.index("-k") + 1] if "-k" in argv else None
    tests = [(n, f) for n, f in globals().items() if n.startswith("test_h") and callable(f)]
    tests.sort(key=lambda t: int(re.match(r"test_h(\d+)", t[0]).group(1)))
    failed = 0
    for name, fn in tests:
        if only and only not in name:
            continue
        t0 = started()
        log_at = os.path.getsize(LOG) if os.path.exists(LOG) else 0
        try:
            fresh()
            fn()
            crashed = started() != t0
            if os.path.exists(LOG):
                with open(LOG, encoding="utf-8", errors="replace") as f:
                    f.seek(log_at)
                    crashed = crashed or "terminated by signal" in f.read()
            if crashed:
                raise Fail("the server crashed or restarted")
            print(f"ok   {name}")
        except Exception as e:  # noqa: BLE001 - report and go on
            failed += 1
            print(f"FAIL {name}: {e}")
    psql(f"DROP DATABASE IF EXISTS {DB} WITH (FORCE);", db="postgres", check=False)
    print(f"{failed} of {len(tests) if not only else 'selected'} hardening tests failed" if failed else "hardening: all passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
