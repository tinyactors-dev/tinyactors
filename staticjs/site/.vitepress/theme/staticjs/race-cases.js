// The programs the front page races: each runs, as written, in staticjs and
// in QuickJS, builds its own data, and must give the same result in both
// before its time is shown. Plain JavaScript that both engines accept.
export const raceCases = [
  {
    id: 'expression',
    name: 'A small expression',
    note: 'one expression',
    source: '((event) => event.type === "order" && event.total > 100 && event.items.some((item) => item.qty > 2))({ type: "order", total: 250, items: [{ qty: 1 }, { qty: 3 }] })',
  },
  {
    id: 'pipeline',
    name: 'Filter and map',
    note: '10,000 orders',
    source: 'Array.from({ length: 10000 }, (_, i) => ({ id: i, total: (i * 37) % 1000, open: i % 3 !== 0 }))\n  .filter((order) => order.open && order.total > 500)\n  .map((order) => order.id)\n  .length',
  },
  {
    id: 'sort',
    name: 'Sort by a key',
    note: '5,000 rows',
    source: 'Array.from({ length: 5000 }, (_, i) => ({ id: i, score: (i * 7919) % 5003 }))\n  .toSorted((a, b) => a.score - b.score)\n  .slice(0, 3)\n  .map((row) => row.id)',
  },
  {
    id: 'group',
    name: 'Group and count',
    note: '10,000 numbers',
    source: 'Object.entries(Object.groupBy(Array.from({ length: 10000 }, (_, i) => i), (n) => ["a", "b", "c", "d"][n % 4]))\n  .map(([key, members]) => [key, members.length])',
  },
  {
    id: 'reduce',
    name: 'Sum with reduce',
    note: '100,000 numbers',
    source: 'Array.from({ length: 100000 }, (_, i) => i % 97).reduce((sum, n) => sum + n, 0)',
  },
  {
    id: 'strings',
    name: 'String work',
    note: '2,000 strings',
    source: 'Array.from({ length: 2000 }, (_, i) => "item-" + i)\n  .join(",")\n  .split(",")\n  .filter((s) => s.endsWith("7"))\n  .length',
  },
  {
    id: 'regex',
    name: 'Regular expression',
    note: '2,000 addresses',
    source: 'Array.from({ length: 2000 }, (_, i) => `user${i}@example.com`)\n  .filter((address) => /^user\\d*7@/.test(address))\n  .length',
  },
  {
    id: 'json',
    name: 'JSON round trip',
    note: '1,000 records',
    source: 'JSON.parse(JSON.stringify(Array.from({ length: 1000 }, (_, i) => ({ id: i, tags: ["a", "b"], ok: i % 2 === 0 })))).length',
  },
];
