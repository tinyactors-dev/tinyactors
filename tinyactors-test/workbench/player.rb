# frozen_string_literal: true

# Ping-pong between two named actors.
#
# Both actors run this one chart; each is bound to the other's address in
# actors.rb, so the only difference between them is who they hit the ball to.
# The send carries a delay, which is what makes the rally steppable: each
# `advance 1s` (or `run`) lets exactly one volley land.
#
# `to_expr:` names a logical address, `#_actor_<family>/<name>`. The system's
# actor directory resolves it, so neither player knows the other's runtime ID.
#
# ---------------------------------------------------------------------------
# Surviving a chart edit
#
# Editing this file replaces both players in place. A replacement is a *fresh*
# interpreter: the data model is carried over, but the active configuration
# starts at the chart's initial state and the delayed send carrying the ball
# dies with the incumbent. A chart that keeps its progress only in its
# configuration therefore stops dead on every edit.
#
# So this one keeps the two facts it needs to resume in the data model, which
# is what survives:
#
#   rallying - a rally is in progress
#   server   - this player started it
#
# After an upgrade both players come back in `idle` holding those. The server
# takes the eventless transition below and puts one new ball in play; the
# other waits for it. Only the server resumes, so exactly one ball is in
# flight no matter how many times the chart is edited mid-rally.
#
# Note where the two halves of a hit live: `on_entry` sends the ball, so every
# way of arriving in `rallying` puts one into play, while the increment sits on
# the transitions, so only a real volley counts and a resume is free. Moving
# the send onto the transition instead is the one change that breaks this
# chart: a serve would enter `rallying` without hitting anything, and the
# rally would never start.
#
# Serve *one* player. `serve` makes that player the server, and only a server
# resumes after an edit. Serving both makes both of them servers, which is two
# rallies with two balls in flight - visible in the logs pane as two hits at
# the same timestamp, going in both directions at once.
TinyactorsTest.chart :player do
  # Overridden per actor by the binding in actors.rb.
  data partner: '#_actor_player/nobody'
  data volleys: 0
  data limit: 15

  # Carried across a chart edit; this is what makes the rally resumable.
  data rallying: false
  data server: false

  initial :idle

  state :idle do
    # An edit interrupted a rally we started: put a new ball in play and
    # carry on from the count we had. Eventless, so it fires as soon as the
    # replacement initializes.
    transition cond: '.rallying and .server', target: :rallying

    # Start a rally.
    transition on: 'serve', target: :rallying do
      assign '.volleys', '1'
      assign '.rallying', 'true'
      assign '.server', 'true'
    end

    # A ball arrived while we were not rallying: the partner served, so join
    # the rally from zero.
    transition on: 'ball', cond: '.rallying | not', target: :rallying do
      assign '.volleys', '1'
      assign '.rallying', 'true'
      assign '.server', 'false'
    end

    # A ball arrived while we were still mid-rally: an edit put us here and
    # the server has resumed. Keep the count.
    transition on: 'ball', cond: '.rallying', target: :rallying
  end

  state :rallying do
    # Every entry into this state puts one ball on its way to the partner: a
    # serve, a join, a return, or a resume after an edit. Only the log and
    # the send live here. The count is not touched, so resuming costs no
    # volley; the transitions below are what count.
    on_entry do
      log 'hit', expr: '"RALLY volley \(.volleys) -> \(.partner)"'
      send_event 'ball', to_expr: '.partner', delay: '1s', id: :ball
    end

    # A ball in flight belongs to the visit that sent it.
    on_exit { cancel :ball }

    # The ball came back: leave and re-enter this state, so on_entry returns
    # it. An external self-transition, which is why it exits at all.
    # A transition's actions run after on_exit and before on_entry, so the
    # new count is the one on_entry logs and hits with.
    transition on: 'ball', cond: '.volleys < .limit', target: :rallying do
      assign '.volleys', '.volleys + 1'
    end


    # At the limit, stop and tell the partner to stop too. Without that the
    # partner's next ball would land on an idle player, which joins a rally,
    # and the two would volley forever.
    transition on: 'ball', cond: '.volleys >= .limit', target: :idle do
      assign '.rallying', 'false'
      log 'rally over', expr: '"\(.volleys) volleys"'
      send_event 'stop', to_expr: '.partner'
    end

    transition on: 'stop', target: :idle do
      assign '.rallying', 'false'
      log 'stopped', expr: '.volleys'
    end
  end
end
