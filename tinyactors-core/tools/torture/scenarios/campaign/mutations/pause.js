campaign.paused = !campaign.paused;
bench.send(campaign.id, campaign.paused ? 'campaign.pause' : 'campaign.resume');
