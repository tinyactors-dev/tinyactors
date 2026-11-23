// Where the reviews live: IndexedDB (the `copywriter` database) in the
// panel's origin, or memory in tests. Both have the same methods, each
// returning a promise. Nothing here ever holds the Gemini key.
//
//   annotations(page?)            every annotation record, or one page's
//   putAnnotation(record)         by (record.page, record.id)
//   deleteAnnotation(page, id)
//   conversations(page) → [record]  the page chat's conversations, {page, id, messages, updatedAt}
//   putConversation(record)         by (record.page, record.id)
//   settings() → object             {model, thinking, voice?, audienceIDs}
//   putSettings(settings)
//   audiences() → [audience]        who reads: {id, name, description, knows?, wants?, tasks?, objections?, source?}
//   putAudience(audience)           by audience.id
//   deleteAudience(id)
//   readings(page?) → [record]     every page's, or one page's, last reading per audience (charts/reading.scxml's record)
//   putReading(record)              by (record.page, record.audienceID)
//
// Version 2 of the database replaced a page's one chat with conversations
// (the old chat is the conversation `c0`) and the personas with the voice
// (an edited default persona's instructions are the voice) and audiences.

const copy = (value) => (value === undefined ? undefined : structuredClone(value));

/** A store in memory, for tests. `dump()` is every record as JSON text. */
export function createMemoryStore() {
  const annotations = new Map();
  const conversations = new Map();
  const audiences = new Map();
  const readings = new Map();
  let settings = {};
  const key = (page, id) => `${page}\n${id}`;
  return {
    async annotations(page) {
      return [...annotations.values()].filter((r) => page === undefined || r.page === page).map(copy);
    },
    async putAnnotation(record) { annotations.set(key(record.page, record.id), copy(record)); },
    async deleteAnnotation(page, id) { annotations.delete(key(page, id)); },
    async conversations(page) { return [...conversations.values()].filter((c) => c.page === page).map(copy); },
    async putConversation(record) { conversations.set(key(record.page, record.id), copy(record)); },
    async settings() { return copy(settings); },
    async putSettings(value) { settings = copy(value); },
    async audiences() { return [...audiences.values()].map(copy); },
    async putAudience(audience) { audiences.set(audience.id, copy(audience)); },
    async deleteAudience(id) { audiences.delete(id); },
    async readings(page) { return [...readings.values()].filter((r) => page === undefined || r.page === page).map(copy); },
    async putReading(record) { readings.set(key(record.page, record.audienceID), copy(record)); },
    dump() {
      return JSON.stringify({
        annotations: [...annotations.values()], conversations: [...conversations.values()], settings, audiences: [...audiences.values()],
        readings: [...readings.values()],
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
    const open = indexedDB.open(name, 3);
    open.onupgradeneeded = (event) => {
      const db = open.result;
      const tx = open.transaction;
      if (event.oldVersion < 1) {
        const annotations = db.createObjectStore('annotations', { keyPath: ['page', 'id'] });
        annotations.createIndex('page', 'page');
        db.createObjectStore('settings', { keyPath: 'key' });
      }
      if (event.oldVersion < 3) {
        const readings = db.createObjectStore('readings', { keyPath: ['page', 'audienceID'] });
        readings.createIndex('page', 'page');
      }
      if (event.oldVersion >= 2) return;
      const conversations = db.createObjectStore('conversations', { keyPath: ['page', 'id'] });
      conversations.createIndex('page', 'page');
      db.createObjectStore('audiences', { keyPath: 'id' });
      if (event.oldVersion === 1) {
        // a page's one chat is its first conversation; an edited default persona is the voice
        tx.objectStore('chats').openCursor().onsuccess = (e) => {
          const cursor = e.target.result;
          if (!cursor) { db.deleteObjectStore('chats'); return; }
          const { page, messages } = cursor.value;
          if (messages?.length) conversations.put({ page, id: 'c0', messages, updatedAt: messages.at(-1)?.at || '' });
          cursor.continue();
        };
        const settings = tx.objectStore('settings');
        tx.objectStore('personas').get('tinyactors-voice').onsuccess = (e) => {
          const voice = e.target.result?.instructions;
          settings.get('settings').onsuccess = (g) => {
            const value = g.target.result?.value || {};
            delete value.personaID;
            if (voice) value.voice = voice;
            settings.put({ key: 'settings', value });
            db.deleteObjectStore('personas');
          };
        };
      }
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
    conversations(page) { return run('conversations', 'readonly', (s) => s.index('page').getAll(page)); },
    putConversation(record) { return run('conversations', 'readwrite', (s) => s.put(record)); },
    async settings() { return (await run('settings', 'readonly', (s) => s.get('settings')))?.value || {}; },
    putSettings(value) { return run('settings', 'readwrite', (s) => s.put({ key: 'settings', value })); },
    audiences() { return run('audiences', 'readonly', (s) => s.getAll()); },
    putAudience(audience) { return run('audiences', 'readwrite', (s) => s.put(audience)); },
    deleteAudience(id) { return run('audiences', 'readwrite', (s) => s.delete(id)); },
    readings(page) { return run('readings', 'readonly', (s) => (page === undefined ? s.getAll() : s.index('page').getAll(page))); },
    putReading(record) { return run('readings', 'readwrite', (s) => s.put(record)); },
    async close() { (await opening).close(); },
  };
}
