Modal with title rule, body and footer — use for confirmations (stop actor, reset system).

```jsx
<Dialog title="Stop checkout.1?" onClose={close} footer={<><Button onClick={close}>Cancel</Button><Button variant="danger">Stop</Button></>}>
  Its 3 children will receive <code>exit</code> and stop.
</Dialog>
```
