mail.outage = true;
bench.after(5000, () => {
  mail.outage = false;
  bench.log('mail processor reachable again');
});
