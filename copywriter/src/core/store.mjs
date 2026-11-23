// Where the reviews live: IndexedDB (the `copywriter` database) in the
// panel's origin, or memory in tests. Both have the same methods, each
// returning a promise. Nothing here ever holds the Gemini key.
//
//   annotations(page?)            every annotation record, or one page's
//   putAnnotation(record)         by (record.page, record.id)
//   deleteAnnotation(page, id)
//   chat(page) → messages         the page-level conversation
//   putChat(page, messages)
//   settings() → object           {model, personaID, ...}
//   putSettings(settings)
//   personas() → [persona]        the user's own; the default is not stored
//   putPersona(persona)           by persona.id
//   deletePersona(id)

const copy = (value) => (value === undefined ? undefined : structuredClone(value));

/** A store in memory, for tests. `dump()` is every record as JSON text. */
export function createMemoryStore() {
  const annotations = new Map();
  const chats = new Map();
  const personas = new Map();
  let settings = {};
  const key = (page, id) => `${page}\n${id}`;
  return {
    async annotations(page) {
      return [...annotations.values()].filter((r) => page === undefined || r.page === page).map(copy);
    },
    async putAnnotation(record) { annotations.set(key(record.page, record.id), copy(record)); },
    async deleteAnnotation(page, id) { annotations.delete(key(page, id)); },
    async chat(page) { return copy(chats.get(page)) || []; },
    async putChat(page, messages) { chats.set(page, copy(messages)); },
    async settings() { return copy(settings); },
    async putSettings(value) { settings = copy(value); },
    async personas() { return [...personas.values()].map(copy); },
    async putPersona(persona) { personas.set(persona.id, copy(persona)); },
    async deletePersona(id) { personas.delete(id); },
    dump() {
      return JSON.stringify({
        annotations: [...annotations.values()], chats: [...chats.entries()], settings, personas: [...personas.values()],
      });
    },
  };
}

const request = (r) => new Promise((resolve, reject) => {
  r.onsuccess = () => resolve(r.result);
  r.onerror = () => reject(r.error);
});

/** The IndexedDB store: the `copywriter` database of `indexedDB`. */
export function createIndexedDBStore({ indexedDB = globalThis.indexedDB, name = 'copywriter' } = {}) {
  const opening = new Promise((resolve, reject) => {
    const open = indexedDB.open(name, 1);
    open.onupgradeneeded = () => {
      const db = open.result;
      const annotations = db.createObjectStore('annotations', { keyPath: ['page', 'id'] });
      annotations.createIndex('page', 'page');
      db.createObjectStore('chats', { keyPath: 'page' });
      db.createObjectStore('settings', { keyPath: 'key' });
      db.createObjectStore('personas', { keyPath: 'id' });
    };
    open.onsuccess = () => resolve(open.result);
    open.onerror = () => reject(open.error);
  });
  // One request in a transaction of its own, which commits when it ends.
  async function run(storeName, mode, fn) {
    const db = await opening;
    const tx = db.transaction(storeName, mode);
    const done = new Promise((resolve, reject) => {
      tx.oncomplete = resolve;
      tx.onerror = () => reject(tx.error);
      tx.onabort = () => reject(tx.error || new Error('IndexedDB aborted the transaction'));
    });
    const result = await request(fn(tx.objectStore(storeName)));
    await done;
    return result;
  }
  return {
    annotations(page) {
      return run('annotations', 'readonly', (s) => (page === undefined ? s.getAll() : s.index('page').getAll(page)));
    },
    putAnnotation(record) { return run('annotations', 'readwrite', (s) => s.put(record)); },
    deleteAnnotation(page, id) { return run('annotations', 'readwrite', (s) => s.delete([page, id])); },
    async chat(page) { return (await run('chats', 'readonly', (s) => s.get(page)))?.messages || []; },
    putChat(page, messages) { return run('chats', 'readwrite', (s) => s.put({ page, messages })); },
    async settings() { return (await run('settings', 'readonly', (s) => s.get('settings')))?.value || {}; },
    putSettings(value) { return run('settings', 'readwrite', (s) => s.put({ key: 'settings', value })); },
    personas() { return run('personas', 'readonly', (s) => s.getAll()); },
    putPersona(persona) { return run('personas', 'readwrite', (s) => s.put(persona)); },
    deletePersona(id) { return run('personas', 'readwrite', (s) => s.delete(id)); },
    async close() { (await opening).close(); },
  };
}
