/**
 * A minimal PostgreSQL client (wire protocol 3.0) for tests, examples and the
 * torture chamber — no dependencies. Extended query protocol with text
 * parameters, LISTEN/NOTIFY, trust or cleartext/md5-less password auth.
 * Not part of any binding: the store link (createStoreLink) takes any
 * client's `query` and `listen`; this one is just enough to run them here.
 *
 *   const db = await connect({ port: 54320, database: 'tinyactors' });
 *   const rows = await db.query('SELECT $1::int + 1 AS n', [41]);   // [{ n: '42' }]
 *   const stop = await db.listen('tinyactors_outbox', (payload) => ...);
 *   await db.end();
 *
 * Values come back as text (or null); jsonb as its JSON text.
 */
import net from 'node:net';

export async function connect({ host = '127.0.0.1', port = 54320, user = 'postgres', database = 'postgres', password = '' } = {}) {
  const sock = net.connect({ host, port });
  await new Promise((ok, no) => {
    sock.once('connect', ok);
    sock.once('error', no);
  });
  sock.setNoDelay(true);
  let buf = Buffer.alloc(0);
  const waiting = []; /* resolvers of the current request: message handlers */
  const channels = new Map();
  let failure = null;
  let handler = null; /* (type, body) for the current request */
  sock.on('data', (chunk) => {
    buf = buf.length ? Buffer.concat([buf, chunk]) : chunk;
    while (buf.length >= 5) {
      const len = buf.readInt32BE(1);
      if (buf.length < 1 + len) break;
      const type = String.fromCharCode(buf[0]);
      const body = buf.subarray(5, 1 + len);
      buf = buf.subarray(1 + len);
      if (type === 'A') {
        /* NotificationResponse: pid, channel, payload */
        const [ch, payload] = cstrings(body.subarray(4));
        const fn = channels.get(ch);
        if (fn) fn(payload);
      } else if (type === 'N' || type === 'S' || type === 'K') {
        /* notices, parameter status, backend key: ignored */
      } else if (handler) handler(type, body);
    }
  });
  const closed = new Promise((ok) => sock.once('close', ok));
  sock.on('error', (e) => {
    failure = e;
    if (handler) handler('!', e);
  });
  sock.on('close', () => {
    failure = failure || new Error('connection closed');
    if (handler) handler('!', failure);
  });

  const send = (type, ...parts) => {
    const body = Buffer.concat(parts.map((p) => (typeof p === 'string' ? Buffer.from(p) : p)));
    const head = Buffer.alloc(type ? 5 : 4);
    if (type) head.write(type, 0);
    head.writeInt32BE(body.length + 4, type ? 1 : 0);
    sock.write(Buffer.concat([head, body]));
  };
  const i16 = (n) => {
    const b = Buffer.alloc(2);
    b.writeInt16BE(n);
    return b;
  };
  const i32 = (n) => {
    const b = Buffer.alloc(4);
    b.writeInt32BE(n);
    return b;
  };
  const z = (s) => Buffer.from(`${s}\0`);

  /* one request at a time: run(fn) where fn(type, body) returns true when done */
  let chain = Promise.resolve();
  const request = (start, onMessage) => {
    const p = chain.then(
      () =>
        new Promise((ok, no) => {
          if (failure) return no(failure);
          let error = null;
          handler = (type, body) => {
            if (type === '!') {
              handler = null;
              return no(body);
            }
            if (type === 'E') error = pgError(body);
            const r = onMessage(type, body);
            if (type === 'Z') {
              handler = null;
              if (error) no(error);
              else ok(r);
            }
          };
          start();
        }),
    );
    chain = p.catch(() => {});
    return p;
  };

  /* startup and authentication */
  await request(
    () => send(null, i32(196608), z('user'), z(user), z('database'), z(database), z('client_encoding'), z('UTF8'), Buffer.from([0])),
    (type, body) => {
      if (type === 'R') {
        const code = body.readInt32BE(0);
        if (code === 3) send('p', z(password));
        else if (code !== 0) throw new Error(`pgwire: authentication method ${code} is not supported (use trust or password)`);
      }
    },
  );

  function query(text, params = []) {
    let fields = [];
    const rows = [];
    return request(
      () => {
        const vals = params.map((v) => (v === null || v === undefined ? null : Buffer.from(typeof v === 'object' ? JSON.stringify(v) : String(v))));
        send('P', z(''), z(text), i16(0));
        send('B', z(''), z(''), i16(0), i16(vals.length), ...vals.flatMap((v) => (v === null ? [i32(-1)] : [i32(v.length), v])), i16(0));
        send('D', 'P', Buffer.from([0]));
        send('E', z(''), i32(0));
        send('S');
      },
      (type, body) => {
        if (type === 'T') {
          const n = body.readInt16BE(0);
          let off = 2;
          fields = [];
          for (let i = 0; i < n; i++) {
            const end = body.indexOf(0, off);
            fields.push(body.toString('utf8', off, end));
            off = end + 1 + 18;
          }
        } else if (type === 'D') {
          const n = body.readInt16BE(0);
          let off = 2;
          const row = {};
          for (let i = 0; i < n; i++) {
            const len = body.readInt32BE(off);
            off += 4;
            row[fields[i]] = len < 0 ? null : body.toString('utf8', off, off + len);
            if (len > 0) off += len;
          }
          rows.push(row);
        }
        return rows;
      },
    );
  }

  return {
    query,
    async listen(channel, fn) {
      channels.set(channel, fn);
      await query(`LISTEN "${channel.replace(/"/g, '""')}"`);
      return async () => {
        channels.delete(channel);
        if (!failure) await query(`UNLISTEN "${channel.replace(/"/g, '""')}"`);
      };
    },
    async end() {
      if (!failure) send('X');
      sock.end();
      await closed;
    },
  };
}

function cstrings(b) {
  const out = [];
  let off = 0;
  while (off < b.length) {
    const end = b.indexOf(0, off);
    if (end < 0) break;
    out.push(b.toString('utf8', off, end));
    off = end + 1;
  }
  return out;
}

function pgError(body) {
  const f = {};
  let off = 0;
  while (off < body.length && body[off] !== 0) {
    const code = String.fromCharCode(body[off]);
    const end = body.indexOf(0, off + 1);
    f[code] = body.toString('utf8', off + 1, end);
    off = end + 1;
  }
  const e = new Error(f.M || 'postgres error');
  e.code = f.C;
  e.detail = f.D;
  return e;
}

/** Is PostgreSQL listening at host:port? */
export function available({ host = '127.0.0.1', port = 54320 } = {}) {
  return new Promise((ok) => {
    const s = net.connect({ host, port });
    s.once('connect', () => {
      s.destroy();
      ok(true);
    });
    s.once('error', () => ok(false));
    s.setTimeout(500, () => {
      s.destroy();
      ok(false);
    });
  });
}
