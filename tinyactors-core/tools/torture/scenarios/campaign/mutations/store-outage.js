store.down = true;
bench.after(6000, () => {
  store.down = false;
  bench.log('store reachable again');
});
