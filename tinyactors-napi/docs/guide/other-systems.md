# Other systems

A host is one **system**, named by its `systemID`. An actor of one system
can send to an actor of another (another host, in this process or on
another machine) by its address, `tinyactors://<systemID>/...`. The host
decides whether an address is its own. A send to another system goes to
your **transport**, a handler that carries it there; the other side
gives it to its host with `host.send`. The transport and the wire are
yours: a socket, a queue, HTTP, a worker's `postMessage`.

## Two systems

Each send to another system reaches the `transport` handler as a `Send`.
Its `target` and `origin` are `Address`es, and its `message` is the
outgoing half, an object of `target`, `event`, `data`, `sendid`,
`origin` and `traceparent`, which the receiving side passes to
`host.send(message)` as it is. On a wire, the message is a value once its
two addresses are strings, so `encode` writes it and `decode` reads it
back:

```js
import { Address, compileCharts, decode, encode, Host } from '@tinyactors/node';

const shopChart = `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="checkout" initial="paying">
  <state id="paying">
    <onentry>
      <send event="charge" target="tinyactors://bank/@teller"><param name="amount" expr="12"/></send>
    </onentry>
    <transition event="approved" target="done"><log expr="'approved by ' + _event.origin"/></transition>
  </state>
  <final id="done"/>
</scxml>`;
const bankChart = `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="teller" initial="open">
  <state id="open">
    <transition event="charge">
      <log expr="'charging ' + _event.data.amount + ' for ' + _event.origin"/>
      <send event="approved" targetexpr="_event.origin"/>
    </transition>
  </state>
</scxml>`;

const image = (name, text) => {
  const compiled = compileCharts([{ name, text }]);
  const bytes = compiled.image;
  compiled.close();
  return bytes;
};

// The wire: bytes in a queue, delivered on the next tick.
const hosts = {};
const wire = (message) => {
  const bytes = encode({ ...message, target: String(message.target), origin: String(message.origin) });
  setImmediate(() => {
    const received = decode(bytes);
    hosts[new Address(received.target).system].send(received);
  });
};

const log = (host, entry) => console.log(entry.text);
hosts.shop = new Host({ systemID: 'shop' }, { log, transport: (host, send) => wire(send.message) });
hosts.bank = new Host({ systemID: 'bank' }, { log, transport: (host, send) => wire(send.message) });
hosts.shop.addImage(image('checkout.scxml', shopChart));
hosts.bank.addImage(image('teller.scxml', bankChart));

hosts.bank.spawn(hosts.bank.chart('teller'), { name: 'teller' });
hosts.shop.spawn(hosts.shop.chart('checkout'));
await new Promise((resolve) => setTimeout(resolve, 50));
await Promise.all([hosts.shop.idle(), hosts.bank.idle()]);
hosts.shop.close();
hosts.bank.close();
```

```output
charging 12 for tinyactors://shop/#_scxml_1
approved by tinyactors://bank/#_scxml_1
```

The receiver answers to `_event.origin`, which is the sender's full
address, so the answer goes back through the bank's transport.

**A send that cannot be delivered** is the transport's to report:
`host.fail(target, sendid)` on the sending host (`send.fail()` while the
handler runs) gives the sender `error.communication` with that `sendid`.
Delivery that worked needs no answer. Without a `transport` handler, each
send to another system is failed.

**An input's target may be an address.** `host.send` takes a session, an
address string or an `Address`; a local address (`tinyactors://shop/@teller`
on the host named `shop`) is resolved by the host to its actor. A message's
`origin` is the sender's address as the other side gave it, and becomes
the event's `_event.origin`.

## Addresses

`new Address(text)` reads an address; its `system`, `session` (a
`bigint`) and `name` are its parts, `undefined` for a part the text
lacks. `Address.fromSession(system, session)` and `Address.fromName(system,
name)` make one, and `toString()` gives the text:

```js
import { Address } from '@tinyactors/node';

const bySession = Address.fromSession('shop', 42n);
const byName = Address.fromName('shop', 'front desk');
console.log(bySession.text, bySession.system, bySession.session);
console.log(byName.text, byName.name);
console.log(new Address('not an address').system);
```

```output
tinyactors://shop/#_scxml_42 shop 42n
tinyactors://shop/@front%20desk front desk
undefined
```

Inside event data an address is plain text: values have no address type.

## SCXML's BasicHTTP

SCXML defines an HTTP processor, BasicHTTP, whose events travel as form
posts. The package has its codec, for an I/O processor or a server of
your own: `encodeHTTPPost({ url, event, fields, body })` writes the
request a send makes, as bytes for a socket, and
`parseHTTPRequest(bytes)` reads an incoming request's event (its
`_scxmleventname` field, or `HTTP.<METHOD>`) and its form fields.

```js
import { encodeHTTPPost, parseHTTPRequest } from '@tinyactors/node';

const request = encodeHTTPPost({
  url: 'http://localhost:8080/scxml',
  event: 'order.placed',
  fields: [{ name: 'sku', value: 'tea' }, { name: 'qty', value: '2' }],
});
console.log(new TextDecoder().decode(request).split('\r\n')[0]);

const incoming = parseHTTPRequest(request);
console.log(incoming.method, incoming.target, incoming.event);
console.log(incoming.fields.map(({ name, value }) => `${name}=${value}`).join(' '));
```

```output
POST /scxml HTTP/1.1
POST /scxml order.placed
sku=tea qty=2
```

The host does no HTTP itself: sending the bytes and serving the port are
your program's.
