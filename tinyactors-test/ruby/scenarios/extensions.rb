# frozen_string_literal: true

# Platform-specific executable content is a `<send>` to a host I/O processor
# (ADR 0046). SCXML 4.10 lets an implementation add executable content of its
# own; appendix G.7 writes the same thing as a `<send>` to a custom event
# processor, and core has no extension action, so this is the whole
# mechanism. What every surveyed engine does — Apache Commons SCXML, SCION,
# uSCXML, Genesys — is report the outcome as an event, because 4.10 forbids an
# extension from changing the configuration any other way.
#
# The chart below is SCXML 4.10's own example, `<ccxml:accept
# connectionid="_event.data.connectionid"/>`, written the sanctioned way:
#
#   the element's namespace  -> the I/O processor type the host registered
#   its local name           -> the event name
#   its attributes           -> <param>s, evaluated by the data model
#   its result               -> an ordinary event (ADR 0046's convention:
#                               `<vocabulary>.<action>.done`, and
#                               `error.platform.<vocabulary>.<action>` for a
#                               failure, which keeps a vocabulary's errors in
#                               the namespace ADR 0027 reserves and lets a
#                               chart catch them with the plain `error`
#                               descriptor)
#
# The chart correlates a reply with a ticket it issues itself and passes as a
# `<param>`, the simplest option. A host may instead correlate on the send ID
# SCXML already defines: it receives `ta_send.send_id` as a number, renders it
# with `ta_send_id_format`, and gets the very text `idlocation` put in the
# chart's data model (ADR 0048).
module CustomActions
  TYPE = 'urn:example:telephony'

  TELEPHONY = TinyactorsTest::Chart.define(:telephony) do
    data ticket: { expr: '0' }, requestid: nil, connection: nil, outcome: nil
    state :idle do
      transition on: 'connection.alerting', target: :accepting do
        assign '.ticket', '.ticket + 1'
        send_event 'accept', type: TYPE, id_location: '.requestid',
                   params: { connectionid: { expr: '$_event.data.connectionid' },
                             ticket: { expr: '.ticket' } }
      end
    end
    state :accepting do
      transition on: 'telephony.accept.done', cond: '$_event.data.ticket == .ticket',
                 target: :connected do
        assign '.connection', '$_event.data.connectionid'
      end
      transition on: 'error', target: :failed do
        assign '.outcome', '$_event.name'
      end
    end
    state :connected
    state :failed
  end
end

TinyactorsTest.scenarios 'custom actions' do
  # Catches: a send that reaches the built-in SCXML processor instead of the
  # vocabulary's, and params that arrive unevaluated. The attribute here is
  # the one SCXML 4.10 itself writes, `connectionid="_event.data.…"`: an
  # extension action carrying an opaque payload could not express it at all,
  # which is half of ADR 0046's argument.
  scenario 'a custom action is a send to its vocabulary processor' do
    io = recording_io(CustomActions::TYPE)
    phone = spawn CustomActions::TELEPHONY
    phone.deliver 'connection.alerting', data: { connectionid: 'c-42' }

    sends = io.entries('send')
    assert_equal 1, sends.size, 'the processor was asked exactly once'
    assert_equal ['accept', CustomActions::TYPE, { connectionid: 'c-42', ticket: 1 }],
                 sends.first.values_at(:event_text, :io_type, :data)
    assert_active phone, :accepting
  end

  # Catches: an `idlocation` that stores nothing on a send to a host
  # processor, which is the chart's only handle on a call in flight (it is
  # what `<cancel sendidexpr>` takes).
  scenario 'the chart is given the ID of the send it made' do
    recording_io(CustomActions::TYPE)
    phone = spawn CustomActions::TELEPHONY
    phone.deliver 'connection.alerting', data: { connectionid: 'c-42' }

    stored = phone.data('.requestid')
    assert stored.is_a?(String) && !stored.empty?, "idlocation stored #{stored.inspect}"
  end

  # Catches: the G.7 loop left open - a completion event that does not reach
  # the waiting chart, or one whose data the chart cannot read. The reply
  # carries the ticket the *processor* was given, which is all a real host
  # knows; nothing here reads the chart's own data to answer it.
  scenario 'the completion event the host sends moves the chart on' do
    io = recording_io(CustomActions::TYPE)
    phone = spawn CustomActions::TELEPHONY
    phone.deliver 'connection.alerting', data: { connectionid: 'c-42' }

    call = io.entries('send').first.fetch(:data)
    phone.deliver 'telephony.accept.done',
                  data: { ticket: call.fetch(:ticket), connectionid: call.fetch(:connectionid) }
    assert_active phone, :connected
    assert_equal 'c-42', phone.data('.connection')
  end

  # Catches: correlation that ignores the ticket, which would let the reply to
  # someone else's call finish this one.
  scenario 'a reply that names another call leaves the chart waiting' do
    io = recording_io(CustomActions::TYPE)
    phone = spawn CustomActions::TELEPHONY
    phone.deliver 'connection.alerting', data: { connectionid: 'c-42' }
    ticket = io.entries('send').first.fetch(:data).fetch(:ticket)

    phone.deliver 'telephony.accept.done', data: { ticket: ticket + 1, connectionid: 'c-99' }
    assert_active phone, :accepting
    assert_equal nil, phone.data('.connection'), 'the foreign reply changed nothing'

    phone.deliver 'telephony.accept.done', data: { ticket: ticket, connectionid: 'c-42' }
    assert_active phone, :connected
    assert_equal 'c-42', phone.data('.connection')
  end

  # The other way to correlate: the ID SCXML defines. The processor holds it
  # as a number and the chart as text, and core renders one into the other
  # (ADR 0048), so the host can answer naming the call the chart knows.
  #
  # Catches: a host and a chart that cannot agree on the identity of a send -
  # the gap ADR 0046 found and ADR 0048 closed. A `ta_send_id_format` that
  # wrote any other text would leave the two sides unable to match.
  scenario 'the host can name the send the chart is waiting for' do
    io = recording_io(CustomActions::TYPE)
    phone = spawn CustomActions::TELEPHONY
    phone.deliver 'connection.alerting', data: { connectionid: 'c-42' }

    # What the processor was handed, rendered the way a chart sees it.
    send_id = io.entries('send').first.fetch(:send_id)
    formatted = core('ta_send_id_format',
                     ta.send.id_format(system: system_handle, send_id: send_id, capacity: 64))
    assert_equal phone.data('.requestid'), formatted.fetch(:buffer),
                 'the host renders the text idlocation gave the chart'

    # And back: the text the chart holds names the send the processor saw.
    parsed = core('ta_send_id_parse',
                  ta.send.id_parse(system: system_handle, text: phone.data('.requestid')))
    assert_equal send_id.to_s, parsed.fetch(:out).to_s, 'parse and format are inverses'
  end

  # Catches: a vocabulary error named outside the error namespace, which a
  # chart listening to `error` would miss (ADR 0027, SCXML 3.12.1).
  scenario 'a vocabulary failure is caught by the plain error descriptor' do
    recording_io(CustomActions::TYPE)
    phone = spawn CustomActions::TELEPHONY
    phone.deliver 'connection.alerting', data: { connectionid: 'c-42' }

    phone.deliver 'error.platform.telephony.accept', data: { reason: 'busy' }
    assert_active phone, :failed
    assert_equal 'error.platform.telephony.accept', phone.data('.outcome')
  end
end
