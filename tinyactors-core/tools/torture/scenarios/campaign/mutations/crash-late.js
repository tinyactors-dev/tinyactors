const wait = Math.round(mail.keyTTLMs * 0.8) + 2000;
bench.removeSystem('mailer');
bench.log(`campaign crashed; restarting in ${wait / 1000} s, after the processor forgot the keys`);
bench.after(wait, () => {
  bench.addSystem('mailer', bench.scenario.systems.mailer);
  campaign.restarts++;
  campaign.start(store.checkpoints.get(campaign.config.campaignID));
});
