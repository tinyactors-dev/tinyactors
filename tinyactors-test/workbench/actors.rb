# frozen_string_literal: true

# Every top-level actor is a named actor, which is what lets a chart edit
# replace it in place instead of restarting the system.

# Two players of the same chart, each bound to the other's address. The
# binding is merged into the chart's declared data, so `volleys` and `limit`
# keep their declared values and only `partner` differs.
Workbench.actor :ping, chart: :player, binding: { 'partner' => '#_actor_player/pong' }
Workbench.actor :pong, chart: :player, binding: { 'partner' => '#_actor_player/ping' }

Workbench.actor :acct1, chart: :subscription
