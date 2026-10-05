# frozen_string_literal: true

# The SCXML Event I/O Processor is built into core (ADR 0040): core resolves
# its targets and envelopes and places the mail itself (ADR 0099),
# rather than making every embedder reimplement SCXML's own processor.
# `ta_system_scxml_io_type()` returns its reserved type atom, which
# `ta_system_register_io` refuses to replace.
#
# SCXML C.1 gives the processor two names: the full URI
# "http://www.w3.org/TR/scxml/#SCXMLEventProcessor" and the short form
# "scxml". An omitted `type` selects it (SCXML 6.2), so all three spellings
# must mean one processor.
#
# "http://www.w3.org/TR/scxml/" is a different name: it is the *invoker*
# type of SCXML 6.4, and core interns it separately. A send addressed to it
# is a send to a processor nobody registered.
module SCXMLProcessor
  URI = 'http://www.w3.org/TR/scxml/#SCXMLEventProcessor'
  SHORT = 'scxml'
  INVOKER_URI = 'http://www.w3.org/TR/scxml/'

  # Sends `hit` to its own internal queue through `type`, and records what
  # came back. The target is `#_internal` so that only the processor's name
  # is under test, not routing.
  def self.chart(type)
    TinyactorsTest::Chart.define(:processor_probe) do
      data outcome: nil
      state :ready do
        on_entry { send_event 'hit', to: '#_internal', type: type }
        transition on: 'hit', type: :internal do
          assign '.outcome', '"hit"'
        end
        transition on: 'error', type: :internal do
          assign '.outcome', '$_event.name'
        end
      end
    end
  end
end

TinyactorsTest.scenarios 'scxml processor' do
  # Catches: an explicit type that takes a different path from an omitted
  # one - a send that is delivered when the author writes nothing but
  # refused when they name the processor SCXML tells them to name.
  scenario 'naming the processor means the same as omitting it' do
    outcomes = [nil, SCXMLProcessor::URI, SCXMLProcessor::SHORT].to_h do |type|
      [type, spawn(SCXMLProcessor.chart(type)).data('.outcome')]
    end
    assert_equal({ nil => 'hit', SCXMLProcessor::URI => 'hit', SCXMLProcessor::SHORT => 'hit' }, outcomes)
  end

  # Catches: the invoker's type quietly doubling as an I/O processor. SCXML
  # 6.4's invoke type is not an Event I/O Processor, so a send addressed to
  # it names a processor nobody registered (test199: an unsupported type is
  # error.execution).
  scenario 'the invoke type is not an event processor' do
    assert_equal 'error.execution', spawn(SCXMLProcessor.chart(SCXMLProcessor::INVOKER_URI)).data('.outcome')
  end

  # Catches: a reserved type a host can take over, which would let an
  # embedder replace SCXML's own processor and change what every `<send>`
  # without a type means.
  scenario 'the reserved type cannot be replaced by a host processor' do
    # A processor that registers for a type of its own, so the refusals
    # below are about the type and not about the adapter.
    io = recording_io('urn:example:other')

    [scxml_io_type, atom(SCXMLProcessor::URI), atom(SCXMLProcessor::SHORT)].uniq.each do |type|
      assert_equal 'err_invalid',
                   ta.system.register_io(system: system_handle, type: type, io: io.adapter)[:result],
                   "registering an I/O processor for atom #{type} is refused"
    end

    # Both spellings name the one reserved processor, and the atom it
    # returns reads back as that URI (ADR 0049), so a host can say which
    # processor a send selected instead of comparing handles.
    assert_equal atom(SCXMLProcessor::URI), scxml_io_type
    assert_equal SCXMLProcessor::URI,
                 core('ta_system_atom_text',
                      ta.system.atom_text(system: system_handle, atom: scxml_io_type)).fetch(:out)
    assert_equal 'hit', spawn(SCXMLProcessor.chart(SCXMLProcessor::SHORT)).data('.outcome'),
                 'the short name still reaches the built-in processor'
  end

  # Catches: a processor that is built in but invisible to a chart.
  # SCXML C.1 requires `_ioprocessors` to carry the processor's location
  # for this session, which is how a chart tells a peer where to reply.
  scenario 'a chart sees the processor and its own location' do
    reader = TinyactorsTest::Chart.define(:ioprocessors_reader) do
      data seen: nil
      state :ready do
        on_entry { assign '.seen', '$_ioprocessors' }
      end
    end

    actor = spawn(reader)
    location = { location: actor.session_uri }
    assert_equal({ SCXMLProcessor::URI.to_sym => location, SCXMLProcessor::SHORT.to_sym => location },
                 actor.data('.seen'))
  end
end
