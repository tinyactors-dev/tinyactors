window.INSPECTOR_CHART = {
  id: 'checkout',
  states: ['cart', 'paying', 'failed', 'paid'],
  final: ['paid'],
  on: {
    cart: { ADD_ITEM: 'cart', CHECKOUT: 'paying' },
    paying: { 'PAYMENT.OK': 'paid', 'PAYMENT.FAIL': 'failed' },
    failed: { RETRY: 'paying', CANCEL: 'cart' },
    paid: {},
  },
};
window.INSPECTOR_TREE = [
  { id: 'root', name: 'app.sup', kind: 'supervisor', tone: 'running', depth: 0 },
  { id: 'sess', name: 'session.sup', kind: 'supervisor', tone: 'running', depth: 1 },
  { id: 'checkout.1', name: 'checkout.1', kind: 'checkout', tone: 'running', depth: 2 },
  { id: 'checkout.2', name: 'checkout.2', kind: 'checkout', tone: 'waiting', depth: 2 },
  { id: 'checkout.3', name: 'checkout.3', kind: 'checkout', tone: 'error', depth: 2 },
  { id: 'mailer', name: 'mailer', kind: 'mailer', tone: 'waiting', depth: 1 },
  { id: 'ledger', name: 'ledger', kind: 'ledger', tone: 'done', depth: 1 },
  { id: 'metrics', name: 'metrics', kind: 'metrics', tone: 'stopped', depth: 1 },
];
