// The process dies: the actor system and every actor in it are gone. The store
// and the mail processor live elsewhere and keep their state.
const cp = store.checkpoints.get(campaign.config.campaignID);
bench.removeSystem('mailer');
bench.log(`campaign crashed; last confirmed checkpoint: ${cp ? `${cp.cursor} claimed, ${cp.sent} sent` : 'none'}`);
bench.after(1000, () => {
  bench.addSystem('mailer', bench.scenario.systems.mailer);
  campaign.restarts++;
  campaign.start(store.checkpoints.get(campaign.config.campaignID));
  bench.log('campaign restarted from its checkpoint');
});
