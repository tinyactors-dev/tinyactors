/**
 * The chess server's PostgreSQL side (optional): finished games and standings
 * are kept by a `records` actor that lives in the database (records.scxml,
 * the tinyactors extension). The server reaches it through a store link —
 * with any PostgreSQL client; here the dependency-free tools/pgwire.mjs.
 *
 *   const records = await connectRecords(ta, system, { port: 54320, database: 'tinyactors' });
 *   records.ref        // an ActorRef to tinyactors://<database>/@records
 *   await records.close();
 */
import { readFile } from 'node:fs/promises';
import { connect } from '../../tools/pgwire.mjs';

const SOURCE = new URL('./records.scxml', import.meta.url);

export async function connectRecords(ta, system, { host = '127.0.0.1', port = 54320, database = 'tinyactors', user = 'postgres' } = {}) {
  const queries = await connect({ host, port, database, user });
  const notices = await connect({ host, port, database, user });
  /* the chart, defined again only when it changed; one records actor */
  const scxml = await readFile(SOURCE, 'utf8');
  await queries.query('CREATE EXTENSION IF NOT EXISTS tinyactors');
  const [current] = await queries.query(
    "SELECT scxml FROM tinyactors.charts WHERE name = 'records' ORDER BY version DESC LIMIT 1",
  );
  if (!current || current.scxml !== scxml) await queries.query("SELECT tinyactors.define('records', $1)", [scxml]);
  await queries.query(
    "SELECT tinyactors.spawn('records', '{}', 'records') WHERE NOT EXISTS (SELECT 1 FROM tinyactors.actors WHERE name = 'records' AND status = 'active')",
  );
  const link = ta.createStoreLink(system, { query: queries.query, listen: notices.listen });
  await link.ready;
  return {
    link,
    ref: system.ref(ta.formatAddress(link.id, 'records')),
    /* a player's standing, straight from the table */
    async standing(name) {
      const [row] = await queries.query(
        "SELECT datamodel->'standings'->$1 AS s FROM tinyactors.actors WHERE name = 'records' AND status = 'active'",
        [name],
      );
      return row && row.s ? JSON.parse(row.s) : null;
    },
    async games() {
      const [row] = await queries.query("SELECT datamodel->'games' AS g FROM tinyactors.actors WHERE name = 'records' AND status = 'active'");
      return row && row.g ? JSON.parse(row.g) : [];
    },
    async close() {
      await link.close();
      await queries.end();
      await notices.end();
    },
  };
}
