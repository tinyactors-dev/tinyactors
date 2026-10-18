// TinyactorsError, as the addon defines it (src/module.c): an Error with
// read-only `code`, `reason`, `path` and `status` (core's name of a
// status, ta_status_name), each undefined when not given. A poisoned
// module's error carries what poisoned it as its `cause`.
export class TinyactorsError extends Error {
  constructor(message, options) {
    const given = options ?? {};
    super(message, given.cause !== undefined ? { cause: given.cause } : undefined);
    Object.defineProperties(this, {
      code: { value: given.code, enumerable: true },
      reason: { value: given.reason, enumerable: true },
      path: { value: given.path, enumerable: true },
      status: { value: given.status, enumerable: true },
    });
  }

  get name() { return 'TinyactorsError'; }
}
