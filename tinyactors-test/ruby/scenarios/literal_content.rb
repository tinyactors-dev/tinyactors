# frozen_string_literal: true

# Literal <content> reaches host adapters as bytes (ADR 0053 section 2).
#
# Who imports: the built-in SCXML Event I/O Processor still hands the
# receiving session a data-model value, so core imports literal content for
# it. A host I/O processor and an invoker receive the chart's bytes and the
# media type the front end gave them, and core does not import: what the
# payload means is platform-specific (SCXML 6.4.2), the adapter knows what it
# wants, and a data model that cannot import content (the null profile) no
# longer stands in the way.
module LiteralContent
  IO = 'urn:tinyactors:test:content-io'
  CLOCK = 'x-clock'
  # Indentation, a trailing newline and a quote: what a data model's
  # whitespace normalization or JSON parse would lose.
  PAYLOAD = "tick:\n  every: 5s\n  \"quoted\"\n"

  SENDER = TinyactorsTest::Chart.define(:content_sender) do
    state :idle do
      transition on: 'now', type: :internal do
        send_event 'payload', type: IO, content: PAYLOAD, media_type: 'text/yaml'
      end
      transition on: 'later', type: :internal do
        send_event 'payload', type: IO, delay: '1s', content: PAYLOAD
      end
      transition on: 'expr', type: :internal do
        send_event 'payload', type: IO, data: '"evaluated"'
      end
    end
  end

  # SCXML 6.4.2's own example is <invoke type="x-clock" src="clock.pl">. Here
  # the clock gets an inline payload instead (SCXML forbids both), under the
  # data model that cannot import content at all.
  CLOCK_CLIENT = TinyactorsTest::Chart.define(:clock_client, datamodel: 'null') do
    state :running do
      invoke type: CLOCK, content: PAYLOAD, media_type: 'text/yaml'
      transition on: 'error.execution', target: :faulted
    end
    state :faulted
  end

  # An inline machine the SCXML invoker cannot read: its media type is not
  # one the invoker has a reader for.
  UNREADABLE_CHILD = TinyactorsTest::Chart.define(:unreadable_child) do
    state :parent do
      invoke type: 'scxml', content: '<scxml/>', media_type: 'text/plain'
      transition on: 'error.execution', target: :refused
    end
    state :refused
  end

  SCXML_RECEIVER = TinyactorsTest::Chart.define(:content_receiver) do
    data got: nil
    state :idle do
      on_entry { send_event 'loop', content: '{"n": 1}' }
      transition on: 'loop', target: :done do
        assign '.got', '$_event.data'
      end
    end
    final :done
  end
end

TinyactorsTest.scenarios 'literal content' do
  # Catches: core importing content for a host processor (the payload would
  # arrive as a data-model value, or whitespace-normalized), and a media type
  # that is dropped on the way.
  scenario 'a host I/O processor receives the bytes and their media type' do
    io = recording_io(LiteralContent::IO)
    sender = spawn LiteralContent::SENDER
    sender.deliver 'now'

    send = io.entries('send').first
    assert_equal [LiteralContent::PAYLOAD, 'text/yaml', nil], send.values_at(:content, :content_type, :data)
  end

  # Catches: a delayed send that forgets its content, or that evaluates it
  # early into a value. The send stores the content's ID, not bytes.
  scenario 'a delayed send delivers the content when it falls due' do
    io = recording_io(LiteralContent::IO)
    sender = spawn LiteralContent::SENDER
    sender.deliver 'later'
    assert_equal [], io.entries('send')

    advance_time 1000
    send = io.entries('send').first
    assert_equal [LiteralContent::PAYLOAD, nil], send.values_at(:content, :content_type)
  end

  # Catches: <content expr> confused with literal content. An evaluated
  # payload is a value, and no bytes accompany it.
  scenario 'an evaluated payload is data, not content' do
    io = recording_io(LiteralContent::IO)
    sender = spawn LiteralContent::SENDER
    sender.deliver 'expr'

    send = io.entries('send').first
    assert_equal ['evaluated', false], [send[:data], send.key?(:content)]
  end

  # Catches: an invoke payload that still goes through import_content, which
  # the null data model refuses, so the invocation would fail before the
  # invoker ever saw it.
  scenario 'an x-clock invocation gets its payload under the null data model' do
    clock = scripted_invoker(LiteralContent::CLOCK)
    client = spawn LiteralContent::CLOCK_CLIENT

    started = clock.started
    assert_equal 1, started.size
    assert_equal [LiteralContent::PAYLOAD, 'text/yaml'], started.first.values_at(:content, :content_type)
    assert_active client, :running
  end

  # Catches: an invoker that guesses what an unlabelled or foreign payload
  # is. The SCXML invoker refuses a media type it has no reader for, and the
  # parent sees error.execution.
  scenario 'the SCXML invoker refuses content it has no reader for' do
    parent = spawn LiteralContent::UNREADABLE_CHILD
    assert_active parent, :refused
  end

  # Catches: the built-in SCXML processor losing its import. The receiving
  # session still reads `_event.data` as a data-model value.
  scenario 'the SCXML Event I/O Processor still imports content' do
    receiver = spawn LiteralContent::SCXML_RECEIVER
    assert_equal({ n: 1 }, receiver.data('.got'))
  end
end
